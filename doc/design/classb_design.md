# LoRaWAN Class B 激活（bring-up）方案

> 版本 V1.0 | 2026-07-18 | 硬件 RAK4630 (nRF52840 + SX1262) | NCS 3.3.0 (Zephyr 4.3.99) | LoRaWAN 1.0.3 OTAA US915
>
> 状态：方案定稿，随后编码实施。本方案是告警多播与 FUOTA（`fuota_design.md` 路线 B）共同的 **P0 级前置项**。
>
> 引用行号基于本地 NCS 树 `g:\NRF_CONNECT_SDK\`（zephyr / modules/lib/loramac-node）。

---

## 1. 背景与问题

实测日志（2026-07-17）：

```
<err> lorawan: Class B not supported yet!
<wrn> hal_sx1262: Set Class B failed: -134 — continuing as Class A
<err> lorawan_mc: MC group 0 RxParams failed: status=3 mac_status=0x04
```

`mac_status=0x04|gid` 已定位为多播 DR3 非法（US915 RX 合法 DR8–13），**已修复为 DR13**（`lorawan_mc.c`）。但 `-134 (-ENOTSUP)` 暴露的是更深的 SDK 能力缺口，共三层：

| 层 | 位置 | 现象 |
|---|---|---|
| ① Zephyr API 层 | `zephyr/subsys/lorawan/loramac-node/lorawan.c:553` | `lorawan_set_class(CLASS_B)` 硬编码 `return -ENOTSUP`，从未实现 |
| ② MLME 事件层 | 同文件 `:234-238` | `mlme_indication_handler` 丢弃**所有** indication，`MLME_BEACON_LOST` 等到不了应用 |
| ③ MAC 编译层 | `zephyr/modules/loramac-node/CMakeLists.txt`（未定义宏）+ `LoRaMacClassB.c:32` | Class B 状态机整文件被 `#ifdef LORAMAC_CLASSB_ENABLED` 包裹，**当前固件里是空 stub** |

**后果**：设备实际以 Class A 运行 → 无 beacon 跟踪、无 ping slot → 即使多播组 "setup OK" 也一包收不到；告警多播和 FUOTA 路线 B 全部被阻塞。

## 2. 方案选型

| 方案 | 做法 | 结论 |
|---|---|---|
| **A. 应用层直调 loramac-node（选定）** | 与 `lorawan_mc.c` 相同的绕行模式：注入编译宏 + 应用层状态机直调 `LoRaMacMlmeRequest`/`LoRaMacMibSetRequestConfirm` | 不改 SDK；所需 API 已逐一核实存在且可链接 |
| B. Fork/patch Zephyr subsys | 给 `lorawan.c` 补 Class B | 违反项目规则（不修改 NCS）；upstream 无现成实现可 backport，工作量与 A 相同 |
| C. 改用 Class C | 一行切换 | 电池设备不可行，设计已否决 |

## 3. 总体设计

新增 `src/hal/lorawan_classb.c`：由**专用 workqueue** 驱动的轮询状态机（阻塞式 `lorawan_send` 不能放系统工作队列）。时序对齐 loramac-node 官方参考实现 `LmHandler.c`（`LmHandlerRequestClass(CLASS_B)` 流程，`LmHandler.c:629-695/809-959`），差异仅一点：**LmHandler 靠 MlmeConfirm/MlmeIndication 回调推进，本模块全部改为轮询**——因为缺口②使事件到不了应用。

```
     lorawan_classb_start()  ← hal_sx1262_join() 成功后调用
            │
            ▼
   ┌─────────────────┐ DeviceTimeReq (+空上行带出)         ┌──────────────────┐
   │   TIME_SYNC     │───────────────────────────────────▶│   ACQUISITION    │
   │ lorawan_request_│  等 15s (覆盖 RX1/RX2 收 Ans)       │ MLME_BEACON_     │
   │ device_time(true)│  有 Ans → 窄窗定点捕获              │ ACQUISITION      │
   └─────────────────┘  无 Ans → 128s 盲扫(1/8 命中,重试)  └────────┬─────────┘
            ▲                                       轮询 5s:        │
            │ 失败退避 60s 重试                IsBeaconModeActive()==true
            │                                                      ▼
   ┌────────┴────────┐  轮询 10s: 尝试 MIB_DEVICE_CLASS   ┌──────────────────┐
   │  (失锁恢复路径)   │  =CLASS_B, SERVICE_UNKNOWN=未就绪  │    PING_SLOT     │
   │ 显式切回 CLASS_A │◀─超时(重发 PingSlotInfo+空上行)──── │ MLME_PING_SLOT_  │
   │ 后回 TIME_SYNC   │                                    │ INFO + 空上行带出 │
   └─────────────────┘                                    └────────┬─────────┘
            ▲                                    MIB set 返回 OK    │
            │ 轮询 30s: IsBeaconModeActive()==false                 ▼
            │ (MAC beacon-less 容忍 2h 后置 LOST 并清状态)  ┌──────────────────┐
            └──────────────────────────────────────────────│     ACTIVE       │
                                                           │  Class B 运行中   │
                                                           └──────────────────┘
```

与现有模块的关系：`hal_sx1262.c` join 成功后调 `lorawan_classb_start()`（替换原来失败的 `lorawan_set_class` 调用）；`lorawan_mc.c` 的 4 个多播组配置照旧（纯配置，Class B 激活后 ping-slot 接收自动生效）。

## 4. 关键机制（全部基于源码核实）

### 4.1 编译宏注入（缺口③）

`firmware/CMakeLists.txt` 加：

```cmake
zephyr_compile_definitions(LORAMAC_CLASSB_ENABLED)
```

- 传播机制：全局定义挂在 `zephyr_interface`，所有 `zephyr_library`（含 loramac-node 模块库，其中 `LoRaMacClassB.c` 本就无条件参编，`zephyr/modules/loramac-node/CMakeLists.txt:50`）与 app 目标同时可见——与现有 `REGION_US915` 的一致性要求同理（`firmware/CMakeLists.txt:34-39` 注释）。
- **ABI 安全性已核实**：`LoRaMacNvmData_t.ClassB`（`LoRaMac.h:798`）与 `LoRaMacClassBNvm.h` 全文均**无条件编译**，开宏不改变任何结构体布局；stub↔实体切换只发生在 `LoRaMacClassB.c` 函数体内部。
- 代价：`static LoRaMacClassBCtx_t Ctx` ≈ 208 B RAM + 约 1.9k 行代码的 ROM。

### 4.2 轮询观测点（替代被丢弃的 MLME 事件）

| 要观测的事 | 轮询手段 | 证据 |
|---|---|---|
| beacon 锁定 / 失锁 | `LoRaMacClassBIsBeaconModeActive()`（extern，声明于内部头 `LoRaMacClassB.h`，未开宏时为恒 false stub） | 锁定置 1：`LoRaMacClassB.c:1375`；LOST 时 `InitClassBDefaults()` 清 0：`:948` |
| 捕获进行中 | `LoRaMacClassBIsAcquisitionInProgress()` | `LoRaMac.c:5421` 自身即用它做重入保护 |
| PingSlotInfoAns 已收 | 不直接观测——**直接尝试切换** `MIB_DEVICE_CLASS=CLASS_B`：返回 `LORAMAC_STATUS_SERVICE_UNKNOWN` 即未就绪，OK 即成功 | 切换要求 `BeaconMode==1 && PingSlotCtx.Assigned==1`（`LoRaMacClassBSwitchClass`，`LoRaMacClassB.c:1527-1550`）；`Assigned` 由 `SRV_MAC_PING_SLOT_INFO_ANS` 置位（`LoRaMac.c:2587-2600`） |

### 4.3 失锁恢复语义（对应规格"失锁 2h 回 Class A"）

- MAC 内部对 beacon 丢失有 **2 小时** 容忍期（`CLASSB_MAX_BEACON_LESS_PERIOD=7200000ms`，`LoRaMacClassBConfig.h:77`），期间窗口指数放宽维持 Class B；超时才进 `BEACON_STATE_LOST`（`LoRaMacClassB.c:846-848`）。
- 关键事实：**LOST 时 MAC 只发 indication（被 Zephyr 丢弃），`DeviceClass` 保持 CLASS_B 不变**——全库仅 `SwitchClass()` 和重新 join 会改它。切回 Class A 是应用职责（LmHandler 也是显式切：`LmHandler.c:923-925`）。
- 本模块 ACTIVE 态 30s 轮询 `IsBeaconModeActive()`，发现 false → 显式 `MIB_DEVICE_CLASS=CLASS_A` → 回 TIME_SYNC 重跑全流程。整体行为恰好实现规格要求（客户确认件：*"return to Class A after beacon locked fail 2 hours"*）。
- 注意：LOST 时 `Assigned` 也被清零，因此恢复必须重新协商 PingSlotInfo——重跑全流程而非仅重捕获。

### 4.4 与 Zephyr 封装共存的三个约束

1. **`mlme_confirm_sem` 假唤醒**：我们直发的 MLME 请求，其 confirm 仍走 `lorawan.c:197` handler 并在 `:231` 无条件 `k_sem_give`（上限 1）；全树唯一 take 在 `lorawan_join`（`:491`）。后果：若 join 与 Class B 流程并发，join 可能读到陈旧状态误判。**约束：本模块仅在 join 成功后启动；运行期不允许再调 `lorawan_join`**（当前工程 rejoin 只发生在复位路径，满足）。此泄漏在原生 `lorawan_request_device_time`/`link_check` 中同样存在，非本方案引入。
2. **`MLME_PING_SLOT_INFO` 要求当前是 CLASS_A**（`LoRaMac.c:5394-5410`）：首轮天然满足；失锁恢复路径必须**先切回 A 再协商**（§4.3 顺序已保证）。
3. **piggyback 带出**：DeviceTimeReq 用 Zephyr 官方 `lorawan_request_device_time(true)`（`lorawan.c:426-445`，`force=true` 自动补空上行）；PingSlotInfoReq 入队后需自发一条空上行带出（对齐 `LmHandler.c:614-621`），用 `lorawan_send(0, "", 0, UNCONFIRMED)`（与 Zephyr `:441` 同款）。`lorawan_send` 阻塞等 MCPS confirm——因此状态机跑在**专用 workqueue**（栈 2 KB），不占系统工作队列。

### 4.5 捕获模式说明

- **有 DeviceTimeAns**（`BeaconDelaySet=1`，`LoRaMacClassB.c:1712`）：窄窗定点捕获（`BEACON_STATE_ACQUISITION_BY_TIME`，`:729-794`），几秒内出结果——主路径。
- **无 Ans（服务器不支持 DeviceTime）**：盲扫模式单次开窗 128s（`:796-823`），而 US915 beacon 在 8 信道跳频（923.3MHz + n×600kHz，`RegionUS915.h:138-153`），单次命中概率约 1/8——靠状态机退避重试可收敛，但平均需 ~17 分钟。**结论：服务器支持 DeviceTimeAns 是体验关键**（§6）。

## 5. NVM 兼容性结论

无迁移动作：开宏不改 NVM 布局（§4.1）；且 Zephyr settings 存取按组做 `sizeof` 校验（`lorawan_nvm_settings.c:93-97`）+ loramac-node 恢复时逐组 CRC32 校验（`LoRaMac.c:3542-3548`），即使未来布局变化旧数据也会被安全丢弃而非错读。

## 6. 网关与服务器依赖（硬前置，需线下确认）

| 依赖 | 现状 | 影响 |
|---|---|---|
| **网关 GPS 授时**（RAK7289 basicstation 发 beacon 的前提） | `doc/App_server/Log/log.txt` 显示 **GPS 无定位** | 不解决则设备永远锁不到 beacon，本方案无法联调——**第一优先排障项** |
| LNS 应答 DeviceTimeReq | 未确认（ChirpStack V4 支持） | 缺失则退化为 128s 盲扫（§4.5），可用但慢 |
| LNS 应答 PingSlotInfoReq + device profile 开启 Class B | 未确认 | 缺失则 `Assigned` 永不置位，无法切 Class B |
| beacon/ping 区域参数 | 设备端零配置（区域代码自动跳频/DR8，`RegionUS915.c:914-930`） | 无风险 |

## 7. 参数与 Kconfig

- 新增 `CONFIG_ALARM_CLASSB_PING_PERIODICITY`：int 0–7，**默认 2**（ping 周期 = 2^N 秒 = 4s，与 4 个静态多播组的 ping 周期一致）。
- 模块内常量（不进 Kconfig）：DeviceTimeAns 等待 15s、捕获轮询 5s、PingSlot 轮询 10s（6 次未就绪重发）、ACTIVE 健康轮询 30s、失败退避 60s。
- 规格中的**自适应 ping 周期**（NORMAL 8s / ALERTED 2s / LOW_BATT 16s，`spec_badge.md` §3.7）需要回 Class A 重新协商 PingSlotInfo，v1 先固定 4s，自适应列为后续迭代。

## 8. 实施清单

| 文件 | 改动 |
|---|---|
| `firmware/CMakeLists.txt` | 追加 `zephyr_compile_definitions(LORAMAC_CLASSB_ENABLED)`（含注释说明传播与 ABI 结论） |
| `firmware/src/hal/lorawan_classb.c` / `.h` | 新增：§3 状态机（专用 workqueue + `k_work_delayable`），对外 `lorawan_classb_start()` / `lorawan_classb_is_active()` |
| `firmware/src/hal/hal_sx1262.c` | join 成功后的 `lorawan_set_class(CLASS_B)` 失败块（`:178-182`）替换为 `lorawan_classb_start()` |
| `firmware/Kconfig` | 新增 `ALARM_CLASSB_PING_PERIODICITY` |
| `doc/design/fuota_design.md` | §11 前置修复清单补 P0-0（本方案），§15 依赖补网关 GPS |

app 的 loramac-node 头文件 include 路径已存在（`firmware/CMakeLists.txt:26-32`，`lorawan_mc.c` 同款），无需新增。

## 9. 测试与验证

日志判据（按序）：

1. 宏生效自检：启动后首个 `MIB_DEVICE_CLASS=CLASS_B` 尝试返回 `SERVICE_UNKNOWN`（未就绪）而非之前的恒失败——说明 stub 已替换为实体。
2. `TIME_SYNC → ACQUISITION`：日志出现 beacon 捕获启动；网关 GPS 正常时数秒内 `IsBeaconModeActive()==true`。
3. `Class B active`（本模块 INF 日志）+ 心跳上行 FCtrl 的 Class B 位由服务器侧确认。
4. 多播实收：服务器向 CODE 组发测试下行 → 设备 ping slot 收到（叠加 DR13 修复后的完整链路验证）。
5. 失锁演练：关网关 beacon（或关 GPS）→ 设备维持 beacon-less Class B 至 2h → 自动回 Class A 并重启捕获流程（日志可见状态迁移）。
6. 构建报告：RAM 增量 ≈ 2.3 KB（Ctx 208B + workqueue 栈 2KB），ROM 增量 ≈ 8–12 KB。

## 10. 风险与待确认

| # | 风险/待确认 | 缓解 |
|---|---|---|
| 1 | 网关 GPS 无定位（硬阻塞） | 与网关部署方先行排障；室内网关需考虑 GPS 天线引出或 NTP-fallback 能力确认 |
| 2 | SimulAlert/ChirpStack 未开 Class B 或不答 DeviceTime/PingSlotInfo | 线下确认 device profile；DeviceTime 缺失时接受盲扫慢启动 |
| 3 | `mlme_confirm_sem` 假唤醒遗留 | §4.4 约束（join 后启动、不重入 join）；长期解法是向 Zephyr upstream 提 Class B 支持 |
| 4 | 盲扫命中率 1/8×128s | 仅为降级路径；主路径依赖 DeviceTimeAns |
| 5 | 自适应 ping 周期未实现 | v1 固定 4s；规格差异已在 §7 声明，列后续迭代 |
