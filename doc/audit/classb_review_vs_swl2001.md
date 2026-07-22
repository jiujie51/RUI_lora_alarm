# LoRaWAN Class B 实现审查报告：以 SWL2001 为参照基准

> 版本 V1.0 | 2026-07-19 | NCS 3.3.0 (loramac-node) vs SWL2001 v4.9.0 (LoRa Basics Modem)
>
> 以 Semtech SWL2001 的 Class B 实现为参照，逐项审查当前项目自研 Class B 激活状态机（`lorawan_classb.c` + loramac-node）

---

## 1. 问题总览

| # | 严重度 | 位置 | 现象 | 影响 |
|---|---|---|---|---|
| **B1** | 🔴 CRITICAL | `lorawan_classb.c:127` `st_acquisition` | `LoRaMacClassBIsBeaconModeActive()` 在 `BEACON_STATE_ACQUISITION_BY_TIME` 时**无条件返回 true**（loramac-node `LoRaMacClassB.c:1459-1461`），一个 beacon 都没收到就报 "Beacon locked" | **伪锁定**：状态机在窄窗打开瞬间即判定锁定，随后空上行 RX 超时 → 状态崩塌 → "假锁→丢" 的全部日志异常得到完整解释 |
| **B2** | 🟡 MED | `lorawan_classb.c:122-125` `st_time_sync` | 不检查时间是否**实际拿到**就进入捕获态；`lorawan_request_device_time(true)` 返回值只反映上行是否发出，不反映 DeviceTimeAns 是否收到 | time-aided 捕获 → 窗口打开 → 没信标 → 退化为盲扫 → 盲扫失败 → 退避重试，整轮空耗 ~4.5min |
| **B3** | 🟡 MED | `lorawan_classb.c:164-167` `st_ping_slot` | 空上行（`lorawan_send`）与 beacon 窄窗并发：loramac-node 上行时仅当 `BeaconMode==1` 才 HaltBeaconing（`LoRaMac.c:3427`+`LoRaMacClassB.c:1481`），伪锁状态下 BeaconMode=0 → 不 Halt → Class A RX1/RX2 超时命中 beacon 分支（`LoRaMac.c:1451`）→ 把 BY_TIME 状态直接推向 LOST | 每轮伪锁后的 PingSlotInfo 空上行会加速（<40s）触发"失锁" |
| **B4** | 🟡 MED | `lorawan_classb.c:129-136` | confirm 失败后，已在应用层 60s 退避重试内又发起新的 `MLME_BEACON_ACQUISITION`——但 **confirm 的 NOT_FOUND 已经把 `BeaconDelaySet` 清零**（`InitClassBDefaults`，`LoRaMacClassB.c:948`），重试的 acquisition 只能做 **128s 单信道盲扫（923.3MHz，1/8 hit）**，不会再 time-aided | 时间辅助每次只能有效一次，重试退化为盲扫 |
| **B5** | 🟢 MIN | `lorawan_classb.c:163` | pingslot_info_send 的空上行发失败（-16 EBUSY）没有重试逻辑 | ping slot 协商在上行碰撞时会丢，需等 10s 轮询周期才能发现未就绪再重发 |
| **B6** | 🟢 MIN | `classb_design.md §3` | 设计文档认为 "BeaconMode==true → 真正锁定"——这与 loramac-node 4.7.0 实现不一致，`IsBeaconModeActive` 有第二条件 | 设计文档错误，误导代码实现 |
| **B7** | 📘 INFO | SWL2001 参照 | SWL2001 有而我们未实现的机制：时间校验前置、DevTReq 凭信标序列、ping window 扩展公式、class b bit 通告、DPLL 限幅锁相等——详见 §3 对比表 | 当前实现能工作但行为脆弱 |

---

## 2. 问题详解（根因 + 代码证据 + 修复）

### B1（CRITICAL）：`IsBeaconModeActive` 的"伪真"第二条件

**根因**：`G:\NRF_CONNECT_SDK\modules\lib\loramac-node\src\mac\LoRaMacClassB.c:1456-1468`：

```c
bool LoRaMacClassBIsBeaconModeActive(void)
{
    if (Ctx.BeaconCtx.Ctrl.BeaconMode == 1) {
        return true;
    }
    // ⚠ 第二条件: 时间辅助捕获进行中、一个 beacon 都没收到
    if (Ctx.BeaconCtx.Ctrl.BeaconState == BEACON_STATE_ACQUISITION_BY_TIME) {
        return true;
    }
    return false;
}
```

`BeaconMode` 的唯一置位点（`LoRaMacClassB.c:1375`）在 `RxBeacon` 内部，要求帧长 23B 且时段 CRC16 校验通过。但 `BEACON_STATE_ACQUISITION_BY_TIME` 在 `MLME_BEACON_ACQUISITION` 请求入口（`LoRaMac.c:5429` + `LoRaMacClassB.c:649-655`）即被设置——只要 `BeaconDelaySet==1`（之前收到过 DeviceTimeAns）就进入此状态。此后 `IsBeaconModeActive` 立即返回 true，**无需收到任何 beacon 帧**。

对照 SWL2001：`lr1mac_core_is_beacon_mode_active`（`lr1mac_core.c:1103-1106`）仅检查 `tx_class_b_bit`（ping slot 就绪后才置），beacon 锁定另有 `smtc_beacon_sniff_get_state` 区分 UNLOCK/LOCKED/SEARCHING，**状态查询与锁定判定不混用**。

**修复方案**（`st_acquisition`）：

将伪锁定的判据从 `LoRaMacClassBIsBeaconModeActive()` 替换为 MlmeConfirm 驱动的判定：等 acquisition 的 confirm 返回 `LORAMAC_EVENT_INFO_STATUS_OK`（这才是真锁）而非 BEACON_NOT_FOUND。loramac-node 的 `mlme_confirm_handler`（Zephyr `lorawan.c:197`）会把 confirm 记入其内部状态，但应用无需依赖它——改为在 acquisition 发起后**轮询 `LoRaMacClassBIsAcquisitionInProgress()` 和上次 confirm 结果**（需通过 `mlme_confirm_sem` 或 `last_mlme_confirm_status` 读取，见下文 4.4 节限制）。

**实际可采用的最小侵入方案**：状态机不依赖轮询 API 判断真锁定。替代：发给 MLME_BEACON_ACQUISITION 后，持续 160s 观察——若 BeaconMode 被 RxBeacon 置 1（真锁），之后的 128s 定时器会维持它；若只是 BY_TIME 伪真，≤128s 后窗口空放→LOST→InitClassBDefaults 清 BeaconMode → `IsBeaconModeActive` 变 false。**在 ACQUISITION 态增加一个"假锁防护延时"——确认被置 true 后至少保持 128s 不变再判为真锁**，过滤掉 BY_TIME 的瞬时伪真。同步改进：接入 MlmeConfirm 结果作为辅助信号。

### B2：未等 DeviceTimeAns 就进捕获

**根因**：`lorawan_request_device_time(true)` 等的是 **MCPS confirm（上行发送完成）**，未必表示 DeviceTimeAns 已到达。时间通过 ClassB.c:1712 `LoRaMacClassBDeviceTimeAns` 生效，该函数在 **DevTimeAnswer 下行处理时**被调用（`LoRaMac.c:2576-2577`），与上行 confirm 异步。

对照 SWL2001：`class_b_mgmt.c:229-234` 持续检查 `lorawan_api_is_time_valid()`，无效则不启动 beacon；管理任务 130s 周期性重发 DeviceTimeReq。即 **时间未就绪绝不进入 beacon 搜索**。

**修复方案**：

`st_time_sync` 在 `lorawan_request_device_time(true)` 后不是等固定 15s 后跳转到 CB_ACQUISITION，而是轮询 `lorawan_device_time_get()`——若时间有效才进捕获，否则在 15s 内重试 DeviceTimeReq（最多 DEVTIME_MAX_TRIES 次），全失败后才作为降级路径进入盲扫捕获。

### B3/B4/B5：并发冲突与重试策略

- **B3**：`st_ping_slot` 的空上行与 beacon 窄窗冲突。方案：在 `pingslot_info_send` 返回 -EBUSY 时不做其他，等下一轮 10s 重试；且在发上行前检查 `LoRaMacClassBIsAcquisitionInProgress()` 是否仍在运行——如果在，等下一轮。
- **B4**：DeviceTimeAns 失效。方案：`st_acquisition` 盲扫失败后回到 `st_time_sync` 时，先请求新的 DeviceTimeReq（而不是直接用缓存的时间进入 time-aided）。已在 B2 修复中一并处理。
- **B5**：增量修复——pingslot_info_send 失败（-EBUSY 或超时）后计数并在下一轮 10s 轮询重试，连续 3 次失败打 ERR 日志。

### B6：设计文档勘误

`classb_design.md §4.2` 应修正对 `LoRaMacClassBIsBeaconModeActive` 的语义描述，补充"存在第二条件，不可用于判定 beacon 锁定"。

---

## 3. SWL2001 vs 当前实现 机制对比表

| 机制 | SWL2001 v4.9.0 | 当前（lorawan_classb.c + loramac-node） |
|---|---|---|
| **beacon 搜索前置条件** | ①时间有效 ②PingSlotInfoReq ACK ③ **两者缺一不可**，补齐前绝不开 RX | 时间 → ACQUISITION 无前置校验；PingSlotInfo 在锁定后才发 |
| **时间有效性检查** | `lr1mac_core_is_time_valid()`，每次开窗前都查；stale=86400s（`LR1MAC_DEVICE_TIME_DELAY_TO_BE_NO_SYNC`） | DeviceTimeAns 到 BeaconDelaySet 无过期检查 |
| **搜索窗口策略** | 仅 **窄窗对准**（初始 ≤2s，`MAX_BEACON_WINDOW_SYMB`），必须 GPS 时间对准 128s 边界 → 无盲扫模式 | time-aided 窄窗 2.12s / 盲扫 128s 连续接收（923.3MHz 单信道，1/8 命中概率） |
| **锁定判定** | `update_beacon_state`：收到帧即 LOCK（首帧即锁，但信标不用于修正时钟——epoch 不回写） | `BeaconMode` 由 RxBeacon CRC1 通过后置 1（+ BY_TIME 状态伪真） |
| **失锁恢复** | 连续 miss 56 拍（2h）或 扩窗到 2s 上限 → UNLOCK；自动回 128s 重捕 + 事件通知；**不自动切回 Class A** | LOST 时 MAC 仅发 indication（被 Zephyr 丢弃）；轮询 `IsBeaconModeActive` 变化 → 应用切 A + 重跑全流程 |
| **信标帧校验** | PHY 层固定长度 + CRC16(0x1021)时段段 + 时间一致性 <2000ms；**但当前版本 is_valid_beacon 恒返回 true（beacon_sniff.c:791-804）** | PHY 层无 CRC（隐式头固定 23B）；9 字节 CRC16 时段段，CRC2 段不阻断锁定；锁定同时将 beacon 时间写入系统时钟 |
| **DPLL 追踪** | 相位修正 ±10ms/拍限幅，频率环慢积分（`BEACON_PLL_FREQUENCY_GAIN=32` 才 ±1ms）；beacon-less 时靠学习到的频率外推 | 无 DPLL；beacon-less 期间靠原始 RTC 走时，窗口按固定公式扩展 |
| **ping slot 窗口** | 锚定 `last_valid_rx_beacon_ms`，每个 slot 按 `elapsed×2×crystal_error` 动态计算窗宽 | beacon 锁定后启动 ping slot（loramac-node 内建），窗口扩展公式由 MAC 处理 |
| **class B bit 通告** | bit 变化时立即发**无 port 空 uplink** 让 NS 知悉设备状态（`class_b_mgmt.c:246-252`） | 无显式通告；ZX 封装层靠上行 FCtrl 的 Class B 位自动指示 |
| **多播会话管理** | `smtc_modem_multicast_class_b_start_session` 要求在 beacon 锁定后才激活；多会话 fpending 优先级仲裁 | 静态预配置 4 组（`lorawan_mc.c`），无运行时会话管理 |
| **恢复间隙** | SWL2001 自身也存在 UNLOCK+时间过期后的重扫链停摆（需要管理服务重开 beacon 服务，但管理服务的 enable 调用仅在 `enabled_get()==false` 时生效——存在一次缝隙） | 已全覆盖（LOST → 切 A → TIME_SYNC 重跑） |

---

## 4. 修复优先级

| 优先级 | 问题 | 修复时间估计 | 前置依赖 |
|---|---|---|---|
| **P0** | B1 伪锁定（核心 bug，现场日志全部异常的解释） | 2h | 无 |
| **P0** | B2 时间前置校验缺失 | 与 B1 同步修复 | 无 |
| **P1** | B3 空上行与 beacon 窗并发 | 1h | B1 修复后伪锁不再出现，此项重要性下降 |
| **P1** | B4/B5 重试策略缺陷 | 1h | 无 |
| **P2** | B6 设计文档勘误 | 30min | 无 |

---

## 5. SWL2001 参照下可借鉴的增强（v2 迭代）

以下机制 SWL2001 已有，当前实现 v1 未覆盖，列为后续迭代：

1. **DPLL 限幅锁相**（防时钟漂移，对长时间 beacon-less 维持质量有提升）——约 200 行代码
2. **ping slot 窗口动态扩展**（依赖 last_valid_beacon_rx_ms 锚点）——对齐规范 RP002，当前 loramac-node 已自带但未经我们测试验证
3. **class B 就绪后空 uplink 通告**（让 NS 同步得知设备已切换 Class B，这对 ChirpStack 正确排程 ping downlink 窗口有帮助）——1 条上行
4. **listen_beacon_rate 省电降采样**——锁定后隔拍听 beacon，减少 RX 功耗
5. **频率运行时可变处理**（BeaconFreqReq/PingSlotChannelReq MAC 命令可改变 beacon 跳频方案）

---

## 6. 附录：执法前验证

B1 最直接的自证方式（不需要动代码）：现场烧录后在 `lorawan_classb.c` 的 `st_acquisition` 函数入口，`IsBeaconModeActive==true` 的 `LOG_INF("Beacon locked")` 之前，加一行防御日志：

```c
LOG_INF("state check: BeaconMode=%u BeaconState=%u",
    /* 需要临时引入 Ctx 符号或只打 IsActive 结果 */);
```

若 BeaconMode==0 且 BeaconState==ACQUISITION_BY_TIME 时打印 "Beacon locked"——即 B1 实证。
