# LoRa 报警系统 10 天开发计划（Hub 优先）

> SDK: nRF Connect SDK **v3.3.0** | 日期: 2026-06-19

---

## 范围确认

| 组件 | 状态 |
|------|------|
| **Hub 固件**（RAK4630 + RAK12501 GPS）| ⬅ 优先开发 |
| **Badge 固件**（RAK4630）| Hub 完成后开发 |
| 网关 RAK7289CV2 | 现成产品，仅配置 |
| 服务器 ChirpStack V4 + Dashboard | 现成产品，仅部署 |

## 关键硬件接口

| 外设 | Hub | Badge |
|------|-----|-------|
| GPS | **UART** 9600bps, NMEA 解析 | 无 |
| LED | **单线级联** (WS2812 兼容, GRB 序, DIN→DO) | **PWM** 6 路 |
| 蜂鸣器 | PWM 高分贝 | PWM 小型 |
| 按键 | 1 个复位键 | 4 个 (R/G/B/Y) GPIO 中断 |
| LCD | 无 | SPI, 双行20字 |
| 振动 | 无 | GPIO MOS 管 |
| BLE | Broadcaster (2s 间隔) | Scanner (4s 报警扫描) |

---

## Day 1: 环境搭建 + 项目骨架 + 协议引擎

**目标**：编译通过，协议帧编解码正确。

| # | 任务 |
|---|------|
| 1.1 | nRF Connect SDK **v3.3.0** 已安装，补充安装 ARM GCC + nrfjprog |
| 1.2 | 创建 `firmware/` 项目：CMakeLists.txt, prj.conf, Kconfig |
| 1.3 | 创建 board 文件：`rak4630_hub.dts`（GPS UART + LED 单线级联 + SX1262 SPI），`rak4630_badge.dts`（LED PWM + 按键 + LCD SPI） |
| 1.4 | `west build -b rak4630_hub` 和 `west build -b rak4630_badge` 编译通过 |
| 1.5 | 实现 `proto_crc16.c` → 验证已知向量 0x58C7 |
| 1.6 | 实现 `proto_parser.c` → 6 状态帧解析器（IDLE→HEAD→HEADER→DATA→DONE/ERROR），512B 缓冲，5s 超时 |
| 1.7 | 实现 `proto_builder.c` → heartbeat(0x00), power(0x01), key_event(0x02) 构建函数 |
| 1.8 | 实现 `proto_handler.c` → `cmd_handlers[256]` 分发表，下行 handler 先留桩 |
| 1.9 | 单元测试：`west build -b native_sim -- -DCONFIG_TEST=y` 全部通过 |

**产出文件**：
```
firmware/CMakeLists.txt, prj.conf, Kconfig
firmware/boards/arm/rak4630/{rak4630_hub.dts, rak4630_badge.dts, board.cmake}
firmware/src/proto/{proto_parser.c, proto_builder.c, proto_handler.c, proto_crc16.c, proto_internal.h}
firmware/tests/unit/{test_proto_crc16.c, test_proto_parser.c}
firmware/src/utils/crc16.c
```

---

## Day 2: 告警状态机 + 执行器管理器

**目标**：8 种告警优先级抢占正确，执行器（LED/蜂鸣器）按模式运行。

| # | 任务 |
|---|------|
| 2.1 | 实现 `alarm_sm.c`：优先级表 P0~P8，`alarm_set()`/`alarm_clear()`，`active_alarms[]` 管理 |
| 2.2 | 抢占规则：Red 不可抢占，All Clear 仅清 Red，Clear All 清全部，Green 重按→新 Red |
| 2.3 | 单元测试覆盖所有状态转换路径 |
| 2.4 | 实现 `actuator_mgr.c`：`mq_actuator` 消费，`alarm_config_t`→时序命令，高优先级抢占低优先级 |
| 2.5 | **Hub LED 驱动** `led_strip.c`：单线级联 WS2812 兼容，Zephyr `led_strip` API，GRB 序，12 灯珠级联帧生成 |
| 2.6 | **Badge LED 驱动** `led_pwm.c`：6 路 PWM，>20kHz |
| 2.7 | `buzzer_pwm.c`：频率 2-4kHz，0-10 音量，60s(Red)/30s(其他) 自动停 |
| 2.8 | `vibration_gpio.c`：GPIO 拉高/拉低（Badge 独有） |
| 2.9 | 硬件验证：串口命令触发 CODE_RED → Hub LED 红灯闪 300ms + 蜂鸣器响 60s |

**产出文件**：
```
firmware/src/app/{alarm_sm.c, alarm_priority.c, actuator_mgr.c}
firmware/src/drv/{led_strip.c, led_pwm.c, buzzer_pwm.c, vibration_gpio.c}
firmware/tests/unit/test_alarm_sm.c
```

---

## Day 3: LoRaWAN Class B 入网 + GPS 驱动

**目标**：Hub 通过 OTAA 入网 ChirpStack。GPS UART 输出有效经纬度。心跳/电量上行送达。

| # | 任务 |
|---|------|
| 3.1 | 实现 `hal_sx1262.c`：SPI 驱动 SX1262，LoRaWAN 1.0.3 Class B OTAA 配置，DevEUI/AppEUI/AppKey 从 Kconfig/Credential 读取 |
| 3.2 | 实现 `lora_thread`（最高优先级）：DIO1 中断→`sem_lora_rx`→proto_parser→`mq_lora_rx`；TX 从 `mq_lora_tx` 取帧 |
| 3.3 | 心跳(5min) + 电量上报(5min+≥5%变化) 定时器在 `sys_workq` 中触发 |
| 3.4 | 自适应 ping slot：NORMAL=4s, ALERTED=2s, 过渡=4s，`LoRaMacSetClassBParam()` 动态调整 |
| 3.5 | 配置 RAK7289CV2 网关：Basic Station→ChirpStack V4，开启 Class B + GPS Beacon |
| 3.6 | 实现 `hal_gps.c`：**UART** 9600bps 中断接收→环形缓冲→行解析 `$GPGGA`/`$GPRMC`→经纬度+有效性 |
| 3.7 | GPS 异常处理：30s 无有效数据→标记失效；室内无信号→lat/lon=无效值；恢复后立即更新 |
| 3.8 | 验证：Hub 入网→ChirpStack 在线→心跳上行→GPS 室外定位有效 |

**产出文件**：
```
firmware/src/hal/{hal_sx1262.c, hal_gps.c, hal_flash.c}
firmware/src/main.c（线程初始化 + 启动流程）
```

---

## Day 4: 下行命令处理 + Hub BLE 广播

**目标**：Hub 处理全部下行命令。BLE 广播每 2s 发送。

| # | 任务 |
|---|------|
| 4.1 | `handle_code()` (CMD 0x03)：GroupID bitmask 匹配 → alarm_sm 输入 |
| 4.2 | `handle_code_setting()` (CMD 0x04)：更新 `alarm_config_t` → Flash |
| 4.3 | 直接控制：CMD 0x05 LED / CMD 0x06 Buzzer / CMD 0x07 Vibration / CMD 0x08 LCD / CMD 0x09 LCD2 |
| 4.4 | `handle_clear_packet()` (CMD 0x0A)：type=0→ClearAll, type=1→AllClear |
| 4.5 | `handle_set_group_id()` (CMD 0x50)：更新 group_id→Flash |
| 4.6 | `ble_hub_adv.c`：SoftDevice Peripheral Broadcaster，2s 间隔，+4dBm，广播数据：Flags + Name "ALARM_HUB" + Manufacturer Data (MAC+dev_type+room_id) |
| 4.7 | 端到端：ChirpStack→CMD 0x03 Code Red→Hub LED 红+蜂鸣→All Clear→LED 绿 |

**产出文件**：
```
firmware/src/ble/{ble_hub_adv.c, ble_utils.c, ble_internal.h}
```

---

## Day 5: Hub 硬件集成 + 电源管理 + 稳定性

**目标**：Hub 固件在真实硬件上功能完整，72h 稳定运行。

| # | 任务 |
|---|------|
| 5.1 | LED 单线级联实测：逻辑分析仪验证 D1 波形 0/1/RESET 时序，12 灯珠全色刷新正常 |
| 5.2 | 高分贝蜂鸣器校准：PWM 频率 2-4kHz 调至最佳声压，0-10 音量线性 |
| 5.3 | Hub 类型区分：RoomHub/DoorHub/HallwayHub 三种 LED+蜂鸣器行为配置和验证 |
| 5.4 | `power_mgr.c`：电池 ADC→电量%，太阳能充电检测，NORMAL/ALERTED/LOW_BATT 模式 |
| 5.5 | `config_store.c`：Flash 最后 4KB，CRC32 校验，工厂复位（按键 10s） |
| 5.6 | WDT 120s 超时，`sys_workq` 每 30s 喂狗，掉电检测 |
| 5.7 | 72h 稳定性挂测：NORMAL 模式，心跳 5min，BLE 2s，检查崩溃/内存泄漏 |

---

## Day 6: Badge 固件 — 按键 + BLE 扫描

**目标**：Badge 共享模块继承自 Hub。按键检测和 BLE 扫描定位工作。

| # | 任务 |
|---|------|
| 6.1 | Badge `main.c`：初始化 4 按键 GPIO + 2 LED PWM + 振动 GPIO + LCD SPI + 充电检测 |
| 6.2 | `button_sm.c`：30ms 消抖，短按<2s，长按 3-5s（Red 二次确认 LCD），组合键 5s |
| 6.3 | 按键→告警映射：Red→Code Red(CMD 0x02)，Blue→Medical，Yellow→Admin，Green→All Clear(仅 Code Red 中) |
| 6.4 | `ble_badge_scan.c`：报警时 4s 连续扫描 Hub 广播→RSSI 中位数→6dB 阈值→选最强 Hub MAC+RSSI |
| 6.5 | 静默定位：NORMAL 下 30s 扫描 1s，房间变化时上报 CMD 0x02(button=0xFF) |
| 6.6 | `lcd_drv.c`：SPI 双行 20 字，背光按键唤醒 3s / 告警时常亮 |

**产出文件**：
```
firmware/src/ui/{button_sm.c, button_combo.c, lcd_drv.c}
firmware/src/ble/{ble_badge_scan.c, ble_locator.c}
```

---

## Day 7: Badge 固件完成

**目标**：Badge 功能完整，全告警场景通过。

| # | 任务 |
|---|------|
| 7.1 | 振动马达 + 全执行器集成：Code Red 时 LED+蜂鸣+振动同时工作 |
| 7.2 | Badge 电池管理：Li-Po ADC 校准，USB-C 充电检测，低电量黄闪 |
| 7.3 | 组合键：Blue+Yellow 5s→复位，Green+Blue 5s→禁用/启用 |
| 7.4 | Badge 下行处理全部 CMD 验证通过 |
| 7.5 | 补充单元测试：`test_button_sm.c`, `test_group_match.c` |
| 7.6 | Badge 全流程：按键→BLE 扫描→LoRa 上行→下行→执行器响应 |

---

## Day 8: Hub + Badge 集成联调

**目标**：多设备场景全部告警交叉验证。

| # | 测试场景 | 预期 |
|---|---------|------|
| 1 | Badge Red 长按 | 所有 Hub 红，Badge 蜂鸣/振动/LCD "CODE RED" |
| 2 | Dashboard All Clear | 所有设备绿 |
| 3 | Badge Blue 长按 | Admin+Nurse 设备蓝，DoorHub 蓝 |
| 4 | Badge Yellow 长按 | Admin 设备黄，DoorHub 黄 |
| 5 | Dashboard Clear All | 全部 Normal |
| 6 | Dashboard Hold | 全部紫 |
| 7 | Hub GPS 上报 | 心跳含有效 lat/lon |
| 8 | 低电量 <30% | Dashboard 通知 |
| 9 | Badge 位置变化 | 房间关联更新 |
| 10 | Red+Blue 同时 | Red 优先，Blue 共存，All Clear 仅清 Red |

边界测试：100 次快速切换告警、断电恢复（config 持久化）、弱 RSSI 场景、GPS 无信号场景。

---

## Day 9: OTA + 离线回退 + 优化

| # | 任务 |
|---|------|
| 9.1 | MCUboot：双 Bank 分区，签名校验，基本启动+回滚 |
| 9.2 | FUOTA：fragment bitmask 持久化，断点续传，CRC32 校验 |
| 9.3 | 离线回退：网关检测断网→本地 ChirpStack→事件队列→恢复同步 |
| 9.4 | 功耗优化：实测 NORMAL/ALERTED/LOW_BATT 电流，对比设计预期 |
| 9.5 | 代码清理：移除调试日志，统一错误码 |

---

## Day 10: 文档 + 量产准备 + 交付

| # | 任务 |
|---|------|
| 10.1 | 固件构建指南 + 网关配置指南 + 设备烧录 SOP |
| 10.2 | 用户操作手册（Badge 按键/LED/充电，Hub 安装/颜色含义） |
| 10.3 | 全量编译 Hub + Badge，烧录 10 Hub + 5 Badge |
| 10.4 | 逐台冒烟测试：上电→入网→心跳→LED/BLE 功能确认 |
| 10.5 | 打 tag `v1.0.0-rc1`，归档固件 + 源码 + 文档 |

---

## 依赖关系图

```
Day1 协议引擎
  ↓
Day2 告警SM + 执行器（Hub LED单线级联 + Badge LED PWM）
  ↓
Day3 LoRaWAN入网 + GPS UART ──── 网关 Class B 配置
  ↓
Day4 下行命令 + BLE Hub广播
  ↓
Day5 Hub硬件集成 + 72h稳定性
  ↓
Day6 Badge按键 + BLE扫描 ← 共享 Day2-4 模块
  ↓
Day7 Badge完成
  ↓
Day8 集成联调 ← Hub+Badge 全部场景
  ↓
Day9 OTA + 离线 + 优化
  ↓
Day10 文档 + 量产 + 交付
```

---

## 裁剪策略（进度落后时）

| 裁剪项 | 影响 |
|--------|------|
| SRP 命令 (Hold/Secure/Evacuate/Shelter) | Dashboard 独有，Badge 不触发 |
| 静默 BLE 定位 (30s) | 保留报警扫描即可 |
| FUOTA | v1.0 USB 烧录 |
| 组合键 | 断电替代 |
| 离线回退 | v1.1 增强 |

---

## 5 天 MVP（仅 Hub 固件交付）

Day3 完成 LoRaWAN+GPS 后即可交付最小 Hub：
- OTAA 入网 + 心跳 + 电量 + Code Red LED+Buzz + BLE 广播 + GPS 坐标
