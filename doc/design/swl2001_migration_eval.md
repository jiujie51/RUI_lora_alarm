# SWL2001 (LoRa Basics Modem) 移植评估：弃用 NCS LoRaWAN 的对比分析

> 版本 V2.0 | 2026-07-19 | SWL2001 v4.9.0 (LoRa Basics Modem MAC 1.0.4) vs NCS 3.3.0 (Zephyr lorawan subsys + loramac-node MAC 1.0.3)
>
> 评估目标：在当前 RAK4630 (nRF52840 + SX1262) 项目上，评估完全弃用 Zephyr lorawan subsys + loramac-node，改用 SWL2001 作为 lib 库（app 直接编译，不依赖 Zephyr 模块集成、最高优先级线程运行 engine）的可行性与成本。

---

## 1. 方案概要

| 项 | 现状（loramac-node 方案） | 候选（SWL2001 方案） |
|---|---|---|
| MAC 版本 | LoRaWAN 1.0.3（`LoRaMac.h`） | LoRaWAN 1.0.4（`lr1mac_defs.h` 硬编码，无降级） |
| Class B / 信标 | Zephyr 封装未实现；应用层自建轮询状态机 + 补编译宏 → **脆弱** | 全内置、LoRa Alliance 认证，Product-grade 状态机 |
| 多播 | 应用层直调 loramac-node 静态预置 4 组（正常工作，DR13 修复后） | `smtc_modem_multicast_set_grp_config` + `_class_b_start_session`，运行时会话管理 |
| FUOTA | 方案阶段，未实施（`fuota_design.md`，TS004 仅收端待建） | 内置 TS003/TS004/TS005 + FMP TS006 + MPA，经分片→flash 回调对接 MCUboot |
| 固件映像 | 227 KB（含 MCUboot 33KB） | 预计 120–180 KB（含 MCUboot），节省 **50–100 KB** flash |
| Zephyr 集成 | 原生 `subsys/lorawan/`（仅 Class A/C） | **不做** Zephyr 模块集成——lib 库直接编入 app，与 loramac-node 我们已用的方式相同（手动 include 路径 + CMake target_link）；HAL 层用 Zephyr API 重写 |
| 许可证 | Zephyr Apache-2.0 | BSD-3-Clause（Clear BSD，无商用限制） |

---

## 2. 技术优劣对照

### 2.1 SWL2001 优势（vs 当前方案）

| # | 优势 | 证据 | 当前状态 |
|---|---|---|---|
| 1 | **Class B 认证质量** | DPLL 限幅锁相 ±10ms/拍、beacon-based 无源时钟外推、2h beacon-less 自适应扩窗、自动重捕 | 自建轮询状态机 + loramac-node；刚修好 B1 伪锁定 bug（`classb_review_vs_swl2001.md`） |
| 2 | **FUOTA 全套内置** | TS003+004+005 v1.0.0/v2.0.0 双版并存 + FMP TS006 + MPA，分片→`CONTEXT_FUOTA` flash 回调，可直对接 MCUboot slot1 | 零；TS004 需从头集成（`fuota_design.md` 规划的 frag_transport 仅收端） |
| 3 | **运行时多播会话管理** | `class_b_start_session`/`stop_session`，ping slot 冲突仲裁、fpending 优先级、独立 fcnt 窗 | 静态预配置 4 组（`lorawan_mc.c`），无运行时会话逻辑 |
| 4 | **省电降采样** | `listen_beacon_rate` 锁定后隔拍听 beacon——电池设备友好 | 无 |
| 5 | **事件驱动的 application integration** | `SMTC_MODEM_EVENT_*` 统一异步事件模型 | 轮询驱动（state-machine + k_work)；因为 Zephyr mlme_indication 被丢弃 |
| 6 | **跨区域可移植性** | 14 个区域完整实现，US915 跳频与 CW 约束由区域抽象统一处理 | loramac-node 同样支持 US915（但 Class B 部分未完全验证） |
| 7 | **ROM/RAM 效率** | 最小 40KB FLASH + 6KB RAM（Class A + 单区域）；全功能预估 120-180KB | 当前 227KB（Class B 侧有 loramac-node 的大区文件 + Zephyr 封装层开销） |

### 2.2 SWL2001 劣势 & 当前方案优势

| # | 劣势/风险 | 严重度 | 影响 |
|---|---|---|---|
| 1 | **HAL 层需从零实现（37 函数）** | 🔴 CRITICAL | 官方仅 bare-metal nRF5 SDK 17.1.0 参考移植（`2_porting_nrf_52840`）；需将所有 `nrf_drv_spi`/`nrf_gpio`/`NRF_RTC1` 寄存器操作翻译为 Zephyr API（`spi_dt_spec`/`gpio_dt_spec`/`counter`）——约 5 人·天 |
| 2 | **协议栈替换代价** | 🟡 MEDIUM | 需重写 4 个关键文件（`hal_sx1262.c`、`lorawan_classb.c`、`lorawan_mc.c` + 头文件），约 450 行应用代码；proto 层不变 |
| 3 | **LoRaWAN 1.0.4 与 ChirpStack v4 / 1.0.3 provision 的兼容性** | 🟡 MEDIUM | DevNonce 行为：SWL2001 严格计数器（L2 1.0.4）、ChirpStack v4 对 1.0.3 设备随机接受；计数器回绕（0xFFFF→0）可能被 ChirpStack 判为重放拒绝 → 需集成测试确认 |
| 4 | **存量设备断代** | 🟡 MEDIUM | 替换协议栈 = 新 NVM 格式 + 新 MAC 版本 + 新参数集 → 无法与当前固件互升 |
| 5 | **最高优先级线程的 CPU 调度影响** | 🟢 LOW | `smtc_modem_run_engine()` 线程设最高优先级，需确保它仅轮询+事件分发不阻塞；SX1262 DIO1 中断处理已在 ISR 上下文中，线程只消费就绪事件——现有 Zephyr 架构已验证同模式 |
| 6 | **无 1.0.3 降级** | 🟢 LOW | 若某些 LNS/网关要求 1.0.3，无回退开关 |
| 7 | **BLE 共存 — RAK4630 双 radio 架构，无冲突** | 🟢 **无风险** | 见 §2.3 |
| 8 | **内部质量缺口** | 🟢 LOW | `is_valid_beacon` 恒 true（`beacon_sniff.c:791-804`）——CRC/时间一致性校验目前不阻断锁定，同样存在假锁风险（虽然窄窗对准策略使其概率远低于 loramac-node）；UNLOCK+时间过期后重扫链停摆缝隙 |

### 2.3 BLE 共存分析：RAK4630 双 radio 架构（🟢 无风险）

**关键事实**：RAK4630 是 `nRF52840 (BLE) + SX1262 (LoRa)` 双芯片双 radio 架构，不是 LR11xx 那样的单芯片共享射频方案。

| 资源 | BLE (MPSL/SoftDevice) | LoRa (SWL2001 + SX1262) | 共享？ |
|---|---|---|---|
| Radio 前端 | nRF52840 内置 2.4 GHz | 外部 SX1262，923 MHz | ❌ |
| SPI 总线 | 无（BLE 走内部 radio，不占 SPI） | SPI1（SX1262 专用） | ❌ |
| 天线 | PCB 天线 / 2.4 GHz 匹配 | 923 MHz 外接天线 | ❌ 频率相隔 1.5 GHz |
| RTC 实例 | RTC0（MPSL timeslot 独占） | **RTC2**（空闲，SWL2001 独占） | ❌ |
| RTC1 | Zephyr 内核 systick（不参与 radio） | 不碰 | ❌ |
| CPU | MPSL 在 RTC0 中断上下文运行 | 最高优先级**线程**（非 ISR） | ⚠️ 仅当 SWL2001 线程长时间关中断才会延迟 MPSL ISR 响应 |

`nRF52840` 有 **5 个 RTC 实例**（RTC0–RTC4），当前 Zephyr + MPSL 分配：
- **RTC0**：MPSL radio planner timeslot 定时
- **RTC1**：Zephyr 内核 systick
- **RTC2**：空闲 → **SWL2001 独占**（`counter` API 映射）

与 LR11xx 不同（单芯片 BLE+LoRa 共用一个射频前端，需要 SWL2001 的 `smtc_modem_external_stack_currently_use_radio()` hook 做 sharing），RAK4630 上该 hook **直接返回 false**——BLE 永远不会占用 SX1262。

因此"BLE 共存"在 RAK4630 上不是移植 gate。**



---

## 3. 移植工作量估算

| 模块 | 工作项 | 人·天 |
|---|---|---|
| HAL (smtc_modem_hal) | 37 函数→Zephyr API 重写：RTC2(counter)/timer(k_timer)/irq(gpio callback)/flash(flash_map)/随机(entropy)/watchdog/TCXO/固件信息/panic — 参考 `2_porting_nrf_52840` 移植逻辑 | 5 |
| Radio BSP | sx126x_hal（SPI+Reset+Busy） + ral_sx126x_bsp（TCXO/RF switch/TxRx 功率参数）→ `spi_dt_spec` + `gpio_dt_spec` | 2 |
| 应用层 API 重写 | `hal_sx1262.c`（init/join/tx/rx event） + `lorawan_mc.c`（multicast 会话） + `lorawan_classb.c` **删除**（SWL2001 内置 Class B） + `main.c` 启停 engine 线程 → smtc_modem_api | 3 |
| 上下文/NVM 布局 | 6 个 CONTEXT_* 各 4KB → 24KB NVM；重新设计 Zephyr 分区表（pm_static.yml + flash_map 映射） | 1.5 |
| 集成测试 | ChirpStack 兼容性（DevNonce/join/Class B/多播/FUOTA）、功耗回归、最高优先级线程调度验证 | 3 |
| **总计** | | **≈ 14.5 人·天** |

**与 V1.0（18.5 人·天）的差异**：砍掉"Zephyr 模块胶水"（2 人·天，不做 Kconfig/CMake 模块/DTS 绑定）；集成测试从 5 减为 3（BLE coexistence 不再需要 PoC，RAK4630 双 radio 天然隔离）。

**最大单体风险**：HAL 层的 `smtc_modem_hal_start_timer` 和 `smtc_modem_hal_get_time_in_ms` 对 RTC 分辨率和中断延迟有要求（100µs 精度，`PORTING_GUIDE.md:11`）。nRF52840 RTC2 @ 32.768kHz 分辨率 ≈ 30.5µs，Zephyr `counter` API 在 tickless 模式下可达此精度——但需实测确认中断延迟不导致 beacon 窗超时。

---

## 4. 总体架构（lib + 最高优先级线程）

```
┌──────────────────────────────────────────────────────────┐
│  app 层（main.c → proto / alarm / badge_ui ...）          │
│  · SMTC_MODEM_EVENT_* 事件回调 → 驱动下行分发 + 状态更新  │
│  · 上行通过 smtc_modem_request_uplink() 同步发起          │
├──────────────────────────────────────────────────────────┤
│  hal_sx1262.c 替换为 SWL2001 API 调用                    │
│  lorawan_mc.c 替换为 smtc_modem_multicast_*              │
│  lorawan_classb.c 删除（SWL2001 内置 Class B）           │
├──────────────────────────────────────────────────────────┤
│  smtc_modem_run_engine() 线程（最高优先级 K_PRIO_COOP(0)）│
│  · 轮询 MAC 事件 → 分发到 app 回调                       │
│  · radio planner 调度 SX1262 TX/RX（DIO1 ISR 触发）      │
│  · 时间关键路径：beacon window / ping slot / RX1/RX2     │
├──────────────────────────────────────────────────────────┤
│  HAL 层（37 函数，app 实现）                              │
│  nrf_drv_spi  → spi_dt_spec（SPI1）                      │
│  nrf_gpio     → gpio_dt_spec（RESET/BUSY/DIO1/TCXO）     │
│  NRF_RTC1     → Zephyr counter（RTC2，32.768kHz）        │
│  nrf_drv_rtc  → k_timer + k_uptime_get                   │
│  nrf_flash    → flash_map API（写入 Zephyr flash 分区）   │
├──────────────────────────────────────────────────────────┤
│  SX1262 (SPI1)              │  nRF52840 内置 2.4GHz       │
│  923MHz LoRa                │  BLE (MPSL/SoftDevice)     │
│  独立 SPI + 专用天线         │  独立 radio + PCB 天线      │
└──────────────────────────────────────────────────────────┘
```

- SWL2001 **不**作为 Zephyr module 集成——与 `loramac-node` 我们已用的方式相同：CMakeLists 手动加 include 路径 + `target_sources` 把 `lbm_lib/` 源文件编入 app。
- `smtc_modem_run_engine()` 线程设 `K_PRIO_COOP(0)`（最高协作优先级），确保 radio 定时事件不被其他线程饥饿。ISR（DIO1/TCXO）直接操作 radio 寄存器，engine 线程只消费 ready 事件，**不阻塞中断**。
- SWL2001 的 `options.cmake`（45 个 LBM_* 开关）裁剪到 US915 + Class A/B + Multicast + FUOTA 最小子集。

## 5. 决策建议

### 推荐走渐进路线（BLE 共存不再是 gate）：

| 阶段 | 内容 | 时机 |
|---|---|---|
| **v1.0 — 维持现状 + 加固** | 维持 loramac-node 方案；完成 Class B 状态机修复（B1/B2 已修复并构建通过）；ChirpStack 多播 DR 同步；网关 GPS → beacon 锁定 → Class B 联通验证 | 当前 |
| **v1.1 — SWL2001 PoC** | 在单独分支实现 HAL + Radio BSP + Class A join → Class B beacon 锁定；验证 ChirpStack 兼容性（DevNonce 1.0.4/1.0.3 互操作）和 RTC2 counter 精度（beacon 窗 ≤2.12s 能否命中）——**≈ 5 人·天** | Class B 稳定运行后 |
| **v1.2 — SWL2001 全面迁移（仅当 PoC 通过）** | 全量替换 HAL + radio + NVM + 应用层 API；利用 SWL2001 内置 FUOTA 替换自研 TS004 收端（届时取代 `fuota_design.md` 路线 B 的大部分设备侧实现） | PoC 通过后 |

**如果在 v1.0 阶段就需要 FUOTA 的 LoRa Alliance 认证**：SWL2001 是唯一经过认证的选项（loramac-node 的 TS004/TS005 包不是 LoRa Alliance 认证范围——Zephyr 服务层是 Zephyr 项目的自研实现）。此场景下建议将 PoC 前置到 v1.0。

---

## 6. 附录：关键文件核对

| 项 | 路径 |
|---|---|
| SWL2001 根目录 | `G:\project\loard_alarm\SWL2001` |
| MAC 版本/区域定义 | `lbm_lib/smtc_modem_core/lr1mac/src/lr1mac_defs.h:56-61`（1.0.4）|
| API 头文件 | `lbm_lib/smtc_modem_api/smtc_modem_api.h` |
| HAL 接口（37 函数） | `lbm_lib/smtc_modem_hal/smtc_modem_hal.h:115-458` |
| Radio BSP 接口 | `lbm_lib/smtc_modem_core/smtc_ral/src/ral_sx126x_bsp.h:91-196` |
| sx126x_hal 接口 | `lbm_lib/smtc_modem_core/radio_drivers/sx126x_driver/src/sx126x_hal.h:70-133` |
| Zephyr module 占位 | `zephyr/module.yml`（仅 `kconfig-ext: true` + `cmake-ext: true`） |
| nRF52840 bare-metal 参考 | `lbm_applications/2_porting_nrf_52840`（nRF5 SDK 17.1.0） |
| options.cmake（LBM_* 开关） | `lbm_lib/options.cmake`（45 个开关） |
| 许可证 | `LICENSE.txt`（Clear BSD = BSD-3-Clause 变体） |
| Class B 激活管理 | `lbm_lib/smtc_modem_core/lorawan_manager/lorawan_class_b_management.c` |
| Class B 信标捕获 | `lbm_lib/smtc_modem_core/lr1mac/src/lr1mac_class_b/smtc_beacon_sniff.c` |
| 上行业务对比审查 | `doc/audit/classb_review_vs_swl2001.md` |
