# Badge（胸牌终端）功能规格与需求

> 版本 V1.0 | 硬件 RAK4630 (nRF52840 + SX1262) | 2026-06-19
>
> 排序规则：定位 > BLE > LoRa 通信 > 告警逻辑 > 执行器 > 按键 > 显示 > 电源

---

## 1. 产品概述

教职工佩戴的便携报警终端。通过 LoRaWAN 与系统通信，BLE 扫描实现室内定位，多模态（LED+蜂鸣+振动+LCD）响应告警。独立于 Wi-Fi 运行。

---

## 2. 硬件规格

| 项目 | 规格 |
|------|------|
| 主控 | nRF52840 (Cortex-M4, 1MB Flash, 256KB RAM) |
| LoRa | SX1262, Class B, 1.0.3, OTAA, ADR |
| GPS | **RAK12501** (u-blox MAX-7Q, **UART** 9600bps, NMEA) |
| BLE | nRF52840 内置 Bluetooth 5.0 |
| 频段 | IN865/EU868/AU915/US915/KR920/RU864/AS923 |

| 外设 | 数量 | 接口 |
|------|------|------|
| RGB LED | 2 颗 | 3ch PWM × 2 |
| 按键 | 4 个 (R/G/B/Y) | GPIO 中断 |
| LCD | 1 块 (双行 20 字) | SPI/I2C |
| 蜂鸣器 | 1 个 | PWM |
| 振动马达 | 1 个 | GPIO (MOS 驱动) |
| 充电 | USB-C | GPIO 检测 |

---

## 3. 功能规格（按重要性排序）

### 3.1 定位功能 ★★★★★

Badge 具备**双模定位**能力：
- **室外**：RAK12501 GNSS 模块直接获取 GPS 坐标
- **室内**：BLE 扫描收集 Hub 广播，RSSI 计算房间级定位

#### 3.1.1 GNSS 定位（室外）★★★★★

RAK12501 GPS 模块为 Badge 提供室外经纬度坐标，通过按键事件上行。

**硬件接口**：

| 参数 | 值 |
|------|-----|
| 模块型号 | RAK12501 (u-blox MAX-7Q) |
| 通信接口 | **UART** (TX/RX) |
| 波特率 | 9600 bps, 8N1 |
| 协议 | NMEA 0183 |
| 解析语句 | `$GPGGA` (定位信息), `$GPRMC` (推荐最小定位) |

**数据格式**：

| 字段 | 长度 | 编码 | 说明 |
|------|------|------|------|
| latitude | 4B | Bit31: N=0,S=1; Bit30-0: 0~90000001 | 90.000000°; 90000001=无效 |
| longitude | 4B | Bit31: E=0,W=1; Bit30-0: 0~180000001 | 180.000000°; 180000001=无效 |

**定位策略**：

| 场景 | GNSS | BLE | 坐标来源 |
|------|------|-----|---------|
| 室外 (GNSS fix) | ✅ 有效 | — | GPS 坐标直接填入 CMD 0x02 |
| 室内 (无 GNSS) | ❌ 无效 | ✅ RSSI | lat/lon=无效值, 用 Hub MAC 定位 |
| GNSS 超时 >30s | ❌ 标记失效 | ✅ RSSI | 回退到 BLE 定位 |

**异常处理**：
- GPS 无信号（室内）：上报 lat/lon = 无效值 (90000001/180000001)
- GPS 超时（>30s 无有效 NMEA）：标记定位失效，切换 BLE 定位
- GPS 恢复：首帧有效后立即更新

#### 3.1.2 BLE 报警定位扫描（室内）

| 参数 | 值 |
|------|-----|
| 触发条件 | 按键确认报警后立即启动 |
| 扫描时长 | 4 秒（100% duty cycle） |
| PHY | 1M |
| 扫描窗口/间隔 | 连续扫描，不休眠 |

#### 3.1.3 RSSI 定位算法

```
输入: 4 秒内收集的所有 Hub 广播 {Hub_MAC, RSSI[], count}
处理:
  1. 丢弃 count < 2 的 Hub（信号不稳定）
  2. 取每个 Hub 的 RSSI 中位数（非均值，抗干扰）
  3. 按中位数 RSSI 降序排列
输出:
  → 最强 RSSI - 第二名 RSSI ≥ 6dB → location_flag = 0x00（确定）
  → 差值 < 6dB                  → location_flag = 0x01（不确定，取最强）
  → 所有 Hub RSSI < -85dBm       → location_flag = 0x02（弱信号）
```

上报格式：CMD 0x02 填入 {最强Hub MAC(6B) + RSSI(1B) + lat(4B) + lon(4B)}
- GNSS 有效时：lat/lon 填入 GPS 坐标
- GNSS 无效时：lat/lon = 无效值，由服务器根据 Hub MAC 查询房间号

#### 3.1.4 静默定位（非报警）

| 参数 | 值 |
|------|-----|
| 扫描间隔 | 30s |
| 扫描窗口 | 1s（低功耗） |
| 上报条件 | 仅当关联 Hub MAC 变化时上报 |
| 上报方式 | CMD 0x02 (button=0xFF, motion=0) |

---

### 3.2 BLE 扫描 ★★★★★

Badge 的 BLE 角色为 **Observer Scanner**（观察者/扫描者），仅接收不发送。

| 模式 | 扫描窗口 | 间隔 | 功耗策略 |
|------|---------|------|---------|
| 报警扫描 | 4s 连续 | 报警触发时 | 全速，不省电 |
| 静默扫描 | 1s | 30s | 低功耗 |
| 低电量 | 禁用 | — | 省电优先 |

> Badge 不使用 BLE 广播功能，不建立 BLE 连接。

---

### 3.3 LoRa 通信 ★★★★★

#### 3.3.1 上行数据（Badge → 服务器，FPort=10）

| CMD | 名称 | 触发 | 数据字段 |
|-----|------|------|---------|
| 0x00 | Heartbeat | 每 5min | device_type(0x00) + group_id |
| 0x01 | Power | 电量变化≥5% 且间隔≥5min | device_type + power_pct(0-100) |
| 0x02 | Key Event | 按键触发+BLE 扫描完成 | button + motion + rssi + hub_mac(6B) + latitude(4B) + longitude(4B) |

#### 3.3.2 下行数据处理（服务器 → Badge，单播 FPort=10, 多播 FPort=20）

| CMD | 名称 | 处理逻辑 |
|-----|------|---------|
| 0x03 | Code | GroupID 匹配 → 告警状态机 |
| 0x04 | Code Setting | 更新 `alarm_config_t` → Flash |
| 0x05 | LED Control | 直接控制（不走状态机） |
| 0x06 | Buzzer Control | 直接控制 |
| 0x07 | Vibration Control | 直接控制 |
| 0x08 | LCD Content | 更新双行文字 |
| 0x09 | LCD Line2 On/Off | 第二行开关 |
| 0x0A | Clear Packet | type=0→ClearAll, type=1→AllClear |
| 0x50 | Set Group ID | 更新 group_id → Flash |

#### 3.3.3 Class B 自适应 Ping Slot

| 模式 | Ping 周期 | 切换条件 |
|------|----------|---------|
| NORMAL | 8s | 默认 |
| ALERTED | 2s | 收到/发送 Code Red 后切换，持续 5min |
| 过渡 | 4s | 5min 后，再 10min 后回 8s |

---

### 3.4 告警状态机 ★★★★

#### 3.4.1 优先级表（P0 最高）

| 优先级 | 告警类型 | 触发源 | 广播范围 |
|--------|---------|--------|---------|
| P0 | Code Red | 红键/Dashboard | 全校 |
| P1 | Shelter (SRP) | Dashboard | 全校 |
| P2 | Evacuate (SRP) | Dashboard | 全校 |
| P3 | Secure (SRP) | Dashboard | 全校 |
| P4 | Hold (SRP) | Dashboard | 全校 |
| P5 | Medical (Blue) | 蓝键 | Admin+Nurse |
| P6 | Admin (Yellow) | 黄键 | Admin |
| P7 | All Clear (Green) | 绿键/Dashboard | 全校 |
| P8 | Normal | — | — |

#### 3.4.2 抢占规则

```
- 高优先级 可抢占 低优先级（Red 不可被任何抢占）
- 同优先级 不重复触发
- All Clear 仅清除 Code Red（Medical/Yellow 保留）
- Clear All 清除全部 → Normal
- Green 状态下再按 Red → 新 Code Red（新源）
```

---

### 3.5 执行器响应 ★★★★

#### 3.5.1 默认告警→执行器映射（可通过 CMD 0x04 远程修改）

| 告警 | LED 颜色 | LED 模式 | 蜂鸣器 | 振动 | LCD |
|------|---------|---------|--------|------|-----|
| Code Red | 红(255,0,0) | 闪 300ms | 常响 60s,vol=10 | 间歇 300ms | "CODE RED" |
| Medical | 蓝(0,100,255) | 常亮 | 闪 500ms,vol=5 | 间歇 500/1500ms | "Medical Alert" |
| Admin | 黄(255,220,0) | 常亮 | 闪 500ms,vol=5 | 间歇 | "Admin Alert" |
| All Clear | 绿(0,255,80) | 闪 1000ms | 关 | 关 | "All Clear" |
| Hold | 紫(180,0,255) | 闪 500ms | 闪 | 间歇 | "Hold Alert" |
| Secure | 蓝(0,100,255) | 闪 | 闪 | 间歇 | "Secure Alert" |
| Evacuate | 绿(0,255,80) | 闪 | 闪 | 间歇 | "Evacuate Alert" |
| Shelter | 橙(255,120,0) | 闪 | 闪 | 间歇 | "Shelter Alert" |

#### 3.5.2 执行器调度规则

- 高优先级告警的执行器命令自动抢占低优先级
- LED/Buzz/Vib 可同时运行（互不冲突）
- 蜂鸣器自动超时断音：Red 60s，其他 30s
- 电量<10% 时：LED 亮度 50%，蜂鸣器音量 50%，振动禁用

#### 3.5.3 LED 颜色规范

| 颜色 | R | G | B | 用途 |
|------|---|---|---|------|
| Red | 255 | 0 | 0 | Code Red |
| Blue | 0 | 100 | 255 | Medical / Secure |
| Yellow | 255 | 220 | 0 | Admin |
| Green | 0 | 255 | 80 | All Clear / Evacuate |
| Purple | 180 | 0 | 255 | Hold |
| Orange | 255 | 120 | 0 | Shelter |

---

### 3.6 按键逻辑 ★★★

#### 3.6.1 按键操作表

| 按键 | 短按 (<2s) | 长按 (3-5s) | 组合键 (5s) |
|------|-----------|-------------|-------------|
| Red | 唤醒 LCD+电量 | **Code Red**（二次确认） | — |
| Blue | 唤醒 LCD+电量 | Medical Alert | Green+Blue=禁用/启用 |
| Yellow | 唤醒 LCD+电量 | Admin Alert | Blue+Yellow=复位 |
| Green | 唤醒 LCD+电量 | **仅 Code Red 中有效**: All Clear | — |

#### 3.6.2 按键状态机

```
IDLE → GPIO中断 → DEBOUNCE(30ms) → PRESS_TRACK
  → t<2s 释放 → SHORT_PRESS
  → 2s<t<5s 释放 (Red) → LONG_PRESS_PENDING → LCD "Hold 2s to confirm"
  → t>5s 释放 → LONG_PRESS_CONFIRMED → 触发报警
  → 双键 5s → COMBO_PRESS
```

#### 3.6.3 Code Red 二次确认

```
Red 长按 3s → LCD 亮 "Hold 2s to confirm" → 倒计时 2s
  → 继续按住 → 触发 Code Red, LCD "CODE RED - SENDING..."
  → 释放 → 取消, LCD "Cancelled"
```

#### 3.6.4 组合键

| 组合 | 功能 | 说明 |
|------|------|------|
| Blue + Yellow (5s) | 复位 | 避免误触 Red |
| Green + Blue (5s) | 禁用/启用 | 禁用后仅 Green+Blue 可重新启用 |

---

### 3.7 显示逻辑 ★★

#### 3.7.1 LCD 状态表

| 场景 | Line1 | Line2 | 背光 |
|------|-------|-------|------|
| 正常 | 熄屏 | 熄屏 | 关 |
| 短按任意键 | Battery: 85% | — | 亮 3s |
| Code Red | CODE RED | 来源房间 | 常亮 |
| Medical Alert | Medical Alert | 房间号 | 常亮 |
| Admin Alert | Admin Alert | 房间号 | 常亮 |
| All Clear | All Clear | — | 常亮 |
| SRP (4种) | [Type] Alert | — | 常亮 |
| 低电量 | Low Battery | 电量% | 闪烁 |

> LCD 可通过 CMD 0x08/0x09 接收 Dashboard 自定义消息。

---

### 3.8 电源管理 ★★

| 模式 | Ping Slot | BLE 扫描 | LED 亮度 | 蜂鸣器 | 振动 | LCD 背光 | 心跳 |
|------|----------|---------|---------|--------|------|---------|------|
| NORMAL | 8s | 30s 被动 1s | 100% | 100% | 100% | 按需 3s | 5min |
| ALERTED | 2s | 4s 全速 | 100% | 100% | 100% | 常亮 | 5min |
| LOW_BATT (<10%) | 16s | 禁用 | 50% | 50% | 禁用 | 3s 超时 | 10min |

| 项目 | 规格 |
|------|------|
| 电池 | 可充电锂电池 |
| 充电 | USB-C, GPIO 检测 |
| 充电指示 | 充电中红灯, 充满绿灯 |
| 低电量告警 | <30% 服务器通知, <10% 本地黄闪 |
| 电量上报 | 变化 ≥5% 且间隔 ≥5min |

---

## 4. 固件架构

### 4.1 线程分配

| 优先级 | 线程 | 触发 | 职责 |
|--------|------|------|------|
| 最高 | lora_thread | sem (DIO1 中断) | LoRa 收发、ping slot、协议解析 |
| 高 | ui_thread | sem (GPIOTE) | 按键检测、消抖、识别 |
| 中 | actuator_thread | sem | LED/Buzz/Vib/LCD 时序 |
| 低 | sys_workq | timer | 心跳、电量、BLE 触发、看门狗 |

### 4.2 启动流程

```
上电 → MCUboot → HW 初始化 → Config Store → OTAA Join
→ Class B Beacon 对齐 → 线程启动 → 首次心跳 → 主循环
```

### 4.3 Flash 分区

```
MCUboot(40KB) | Slot-0(460KB) | Slot-1(460KB) | Config(4KB) | Scratch(60KB)
```

---

## 5. Code Red 完整数据流（核心场景）

```
1. 用户长按 Red 3s → LCD "Hold 2s to confirm"
2. 继续按 2s → 确认触发
3. BLE 扫描 4s → 收集 Hub RSSI → 选最强 Hub MAC
4. LoRa TX: CMD 0x02 {button=Red, long_press, rssi, hub_mac, lat, lon}
5. 服务器收到 → 查询 Hub 房间号 → 组播 CMD 0x03 {GroupID=0xFF, Alarm=Red}
6. 所有设备在 ping slot 收到 → 告警状态机切到 CODE_RED
7. Badge 执行器: LED 红闪 + 蜂鸣 60s + 振动间歇 + LCD "CODE RED"
8. 等待 All Clear / Clear All
```

---

## 6. 与 Hub 差异

| 特性 | Badge | Hub |
|------|-------|-----|
| 按键 | 4 个 | 1 个复位 |
| LED | 2 颗小型 | 多颗高亮 MOSFET |
| 蜂鸣器 | 小型 | 高分贝 |
| 振动 | **有** | 无 |
| LCD | **有** | 无 |
| BLE | Scanner | Broadcaster |
| GPS | **有** (RAK12501) | 无 |
| 供电 | USB-C 充电 | 电池+太阳能 |
