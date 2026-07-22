# SWL2001 移植后续待办清单

> 2026-07-19 | 全部 5 个 Phase 构建通过，Badge + Hub 双变体 `merged.hex` 已生成。

## P0 — 阻塞项

| # | 任务 | 描述 | 依赖 |
|---|---|---|---|
| 14 | **烧录实测** | RAK4630 烧录 SWL2001 固件，通过 ChirpStack 验证：OTAA join 成功、Class B beacon 锁定、4 个多播组 ping-slot 下行实收、心跳上行可见 | 15, 16 |
| 15 | **ChirpStack 多播 DR 同步** | 设备端已修复 `MC_DATARATE=13`，SimulAlert/ChirpStack 侧 4 个组的 DR 需同步从"DR3"改为 DR13（SF7/BW500，923.3MHz）；`muticast_flow.md` 勘误 | — |
| 16 | **网关 GPS 授时排障** | RAK7289 basicstation 发 Class B beacon 需要 GPS 授时；当前日志显示 GPS 无定位 → 设备永远锁不到 beacon。与网关部署方排查 GPS 天线/室内覆盖 | — |

## P1 — 分区与存储

| # | 任务 | 描述 | 依赖 |
|---|---|---|---|
| 17 | **pm_static.yml 分区固化** | 按 `fuota_design.md` §5 设计正式分区表替换临时 0xF0000 Context Flash 映射；断代变更需有线重刷存量设备 + ChirpStack 清 DevNonce | 14 |

## P2 — 优化项

| # | 任务 | 描述 | 依赖 |
|---|---|---|---|
| 18 | **RTC2 counter 精度提升** | beacon 窗口需要 100µs 精度，当前 `k_uptime_get` 仅 1ms；改用 Zephyr `counter` API 驱动 RTC2 实现 30.5µs 精度 | 14 |
| 19 | **DIO1 GPIOTE 中断** | 当前 engine 线程靠 `k_sem_take` 超时唤醒，改成 DIO1 GPIOTE 上升沿中断 → `k_sem_give` 即时唤醒，减少 radio 响应延迟 | 14 |
| 20 | **FUOTA 设备侧实现** | SWL2001 已内置 FUOTA 引擎，需实现：(1) `CONTEXT_FUOTA` → flash_map 写 `mcuboot_secondary`；(2) `SMTC_MODEM_EVENT_LORAWAN_FUOTA_DONE` 后 reboot + `boot_write_img_confirmed`；(3) `tools/fuota_class_b_sender.py` 下发脚本 | 17 |

## 已完成

| Phase | 内容 |
|---|---|
| 1 | SWL2001 lib 编译 + 37 函数桩 HAL + CMake/Kconfig |
| 2 | SPI/GPIO 硬件 HAL（nrf_gpio 直接操作，1MHz SPI1） |
| 3 | RTC 定时器 + engine 线程 K_PRIO_PREEMPT(1) 8KB 栈 |
| 4 | OTAA join + engine 事件回路 + `SMTC_MODEM_EVENT_JOINED/DOWNDATA` |
| 5 | 多播组 API + Hub 构建 + Context Flash 存储 + 旧代码守卫 |
