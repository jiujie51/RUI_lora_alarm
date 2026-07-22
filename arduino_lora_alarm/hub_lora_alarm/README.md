# LoRa Alarm System — Arduino/RUI3 Firmware

基于 RUI3 (RAK Unified Interface V3) 的 LoRa 报警系统嵌入式固件。

## 设备类型

| 设备 | .ino 文件 | 描述 |
|------|----------|------|
| **Hub** | `hub_lora_alarm.ino` | 集线器：WS2812 LED 灯带 (15 颗) + 蜂鸣器 + BLE 广播 |
| **Badge** | `badge_lora_alarm.ino` (待实现) | 胸牌：RGB LED + OLED + 4 按键 + 蜂鸣器 + 振动 + GPS + BLE 扫描 |

## 目录结构

```
arduino_lora_alarm/
├── hub_lora_alarm.ino          # Hub 主程序
├── badge_lora_alarm.ino        # Badge 主程序 (待实现)
├── debug_macros.h              # 调试宏 (LOG_INFO/WARN/ERROR)
├── boards/
│   ├── hub/board.h             # Hub 引脚 + 凭证配置
│   └── badge/board.h           # Badge 引脚 + 凭证配置
├── app/
│   ├── alarm_sm.h/cpp          # 告警状态机
│   ├── actuator_mgr.h/cpp      # 执行器管理器
│   ├── power_mgr.h/cpp         # 电源管理
│   ├── join_state.h            # 入网状态枚举
│   └── app_hal.h/cpp           # RUI3 LoRaWAN 抽象层
├── proto/
│   ├── proto_internal.h        # 协议常量
│   ├── proto_parser.cpp        # 帧解析器
│   ├── proto_builder.cpp       # 帧构建器
│   ├── proto_handler.cpp       # 命令处理器
│   └── proto_crc16.cpp         # CRC16/XMODEM
├── config/
│   └── config_store.h/cpp      # Flash 配置存储
├── ble/
│   ├── ble_hub_adv.h/cpp       # Hub BLE 广播
│   └── ble_badge_scan.h/cpp    # Badge BLE 扫描 (待实现)
├── drv/
│   ├── led_strip.h/cpp         # WS2812 灯带 (Hub)
│   ├── led_pwm.h/cpp           # PWM LED (Badge, 待实现)
│   ├── buzzer_pwm.h/cpp        # 蜂鸣器 (nrfx_pwm)
│   ├── oled_drv.h/cpp          # OLED (Badge, 待实现)
│   └── vibration_gpio.h/cpp    # 振动马达 (Badge, 待实现)
├── ui/
│   ├── button_sm.h/cpp         # 按键状态机 (Badge, 待实现)
│   └── badge_ui.h/cpp          # Badge UI (待实现)
├── hal/
│   └── hal_gps.h/cpp           # GPS NMEA 解析 (Badge, 待实现)
└── utils/
    └── crc32.h/cpp             # CRC32
```

## 编译

使用 Arduino IDE 或 Arduino CLI，目标板选择 **WisCore RAK4631 Board**。

### Hub
1. 打开 `hub_lora_alarm.ino`
2. 选择工具 → 开发板 → WisCore RAK4631 Board
3. 上传

### Badge (待实现)

## 关键架构

- **Protothreads**: 协程式多任务, `loraThread` (入网+TX) + `actuatorThread` (10ms tick)
- **LoRaWAN**: RUI3 `api.lorawan.*` — OTAA Class B US915
- **协议**: 自定义二进制协议 0xAA55 帧头 + CRC16/XMODEM
- **Flash 存储**: RUI3 `api.system.flash` — 4 槽 16KB (Primary/Backup1/Backup2/Factory)

## 参考文档

- `docs/RUI3_Arduino_Porting_Plan.md` — 完整移植方案
- `docs/RUI3_Concurrency_Analysis.md` — 并发模型分析
- `doc/design/Lora_Alarm_System_Application_Communication_Protocol.md` — 协议文档 V1.4
