# Hub（集线器）功能规格与需求

> 版本 V1.1 | 硬件 RAK4630 | 2026-06-23
>
> 排序规则：BLE 广播 > LoRa 通信 > 告警逻辑 > 执行器 > 按键 > 电源

---

## 1. 产品概述

Hub 是安装在教室/走廊/门外的固定报警指示设备。通过 BLE 广播为 Badge 提供室内定位基准，同时通过高亮 LED 和蜂鸣器指示当前告警状态。Hub 为固定安装设备，位置在部署时已知，无需 GNSS。

### 1.1 三种安装类型

| 类型 | 安装位置 | 安装方式 |
|------|---------|---------|
| **RoomHub** | 教室/房间内 | 天花板安装 |
| **DoorHub** | 教室门外上方或旁侧 | 壁挂安装 |
| **HallwayHub** | 走廊沿线 | 天花板安装 |

---

## 2. 硬件规格

| 项目 | 规格 |
|------|------|
| 主控 | nRF52840 (Cortex-M4, 1MB Flash, 256KB RAM) |
| LoRa | SX1262, Class B, 1.0.3, OTAA, ADR |
| BLE | nRF52840 内置 Bluetooth 5.0 |
| 频段 | IN865/EU868/AU915/US915/KR920/RU864/AS923 |

| 外设 | 数量 | 接口 | 说明 |
|------|------|------|------|
| 高亮 RGB LED | 多颗 | **单线级联** (DIN→DO) | 单 GPIO 控制，级联转发，6 色 |
| 高分贝蜂鸣器 | 1 个 | PWM | Code Red 60s 自动停 |
| 复位键 | 1 个 | GPIO | 仅复位，无功能按键 |
| 电池 | 大容量 + 太阳能 | ADC 采样 | Li-SOCl₂ D 型 19000mAh 或可充电 |
| 看门狗 | 内部 WDT | — | 120s 超时 |

> Hub 无 LCD、无振动马达、无功能按键（仅复位键）。

---

## 3. 功能规格（按重要性排序）

### 3.1 BLE 广播 ★★★★★

Hub 的 BLE 角色为 **Peripheral Broadcaster**（外设/广播者），为 Badge 定位提供信号基准。

#### 3.1.1 广播参数

| 参数 | 值 |
|------|-----|
| 广播间隔 | **2 秒**（兼顾定位精度与功耗） |
| TX Power | **+4dBm**（nRF52840 内置 PA） |
| 有效距离 | 20~40m（室内） |
| PHY | 1M |

#### 3.1.2 广播数据包（31 字节）

```
[Flags: 0x06] [Complete Local Name: "ALARM_HUB"] [Manufacturer Data: {MAC(6B) + device_type(1B) + room_id(1B)}]
```

- `device_type`: 0x01 = Hub
- `room_id`: 配置的房间编号

#### 3.1.3 运行模式

| 模式 | 广播间隔 | 说明 |
|------|---------|------|
| NORMAL | 2s | 正常广播 |
| LOW_BATT | 4s | 省电模式，延长间隔 |

> Hub 不建立 BLE 连接、不扫描、无需配对。广播由 SoftDevice 自主运行，CPU 仅在更新广播数据时介入。

---

### 3.2 GPS 定位

Hub 为固定安装设备，GPS 坐标在部署时由安装人员记录并录入服务器数据库。Hub 本身不集成 GNSS 模块（RAK12501 已移至 Badge 用于移动定位）。

---

### 3.3 LoRa 通信 ★★★★★

#### 3.3.1 上行数据（Hub → 服务器，FPort=10）

| CMD | 名称 | 触发 | 数据字段 |
|-----|------|------|---------|
| 0x00 | Heartbeat | 每 5min | device_type(0x01) + group_id |
| 0x01 | Power | 电量变化≥5% 且间隔≥5min | device_type + power_pct(0-100) |

> Hub 无按键，不上报 CMD 0x02（除非将来集成按键），GPS 坐标可嵌入 CMD 0x00 扩展字段或通过 CMD 0x02 上报。

#### 3.3.2 下行数据处理（服务器 → Hub，单播 FPort=10, 多播 FPort=20）

| CMD | 名称 | 处理逻辑 |
|-----|------|---------|
| 0x03 | Code | GroupID 匹配 → 告警状态机 |
| 0x04 | Code Setting | 更新 `alarm_config_t` → Flash |
| 0x05 | LED Control | 直接控制 LED |
| 0x06 | Buzzer Control | 直接控制蜂鸣器 |
| 0x0A | Clear Packet | type=0→ClearAll, type=1→AllClear |
| 0x50 | Set Group ID | 更新 group_id → Flash |

> Hub 无 LCD/振动，CMD 0x07/0x08/0x09 忽略。

#### 3.3.3 Class B 自适应 Ping Slot

| 模式 | Ping 周期 | 切换条件 |
|------|----------|---------|
| NORMAL | 8s | 默认 |
| ALERTED | 2s | 收到 Code Red 后切换，持续 5min |
| 过渡 | 4s | 5min 后再 10min 后回 8s |

---

### 3.4 告警状态机 ★★★★

与 Badge 共享相同告警状态机，优先级表一致：

| 优先级 | 告警类型 | Hub LED 行为 |
|--------|---------|-------------|
| P0 | Code Red | 红灯闪烁 |
| P1 | Shelter (SRP) | 橙灯闪烁 |
| P2 | Evacuate (SRP) | 绿灯闪烁 |
| P3 | Secure (SRP) | 蓝灯闪烁 |
| P4 | Hold (SRP) | 紫灯闪烁 |
| P5 | Medical (Blue) | 蓝灯（仅 DoorHub 显示来源房间告警） |
| P6 | Admin (Yellow) | 黄灯（仅 DoorHub 显示来源房间告警） |
| P7 | All Clear | 绿灯闪烁 |
| P8 | Normal | LED 全灭 |

---

### 3.5 三种 Hub 类型的执行器行为 ★★★★

#### 3.5.1 RoomHub

| 告警状态 | LED | 蜂鸣器 |
|---------|-----|--------|
| Code Red | 红灯闪烁 | 60s |
| All Clear | 绿灯闪烁 | 关闭 |
| Clear All / Normal | 全灭 | 关闭 |

#### 3.5.2 DoorHub

| 告警状态 | LED | 蜂鸣器 | 说明 |
|---------|-----|--------|------|
| Code Red | 红灯闪烁 | 60s | 全校广播 |
| All Clear | 绿灯闪烁 | 关闭 | 全校广播 |
| Medical（本房间） | **蓝灯**常亮 | 30s | 仅本房间触发 |
| Admin（本房间） | **黄灯**常亮 | 30s | 仅本房间触发 |
| Clear All / Normal | 全灭 | 关闭 | — |

> DoorHub 区分"全校广播"和"本房间触发"，Medical/Admin 仅当来源是本房间时才亮对应颜色。

#### 3.5.3 HallwayHub

| 告警状态 | LED | 蜂鸣器 | 说明 |
|---------|-----|--------|------|
| Code Red | 红灯闪烁 | 60s | **持续到 All Clear** |
| All Clear | 绿灯闪烁 | 关闭 | — |
| Clear All / Normal | 全灭 | 关闭 | — |

> HallwayHub 在 Code Red 期间持续亮红，不因 Clear All 而灭（仅 All Clear 可解除）。

---

### 3.6 执行器规格 ★★★

#### 3.6.1 高亮 LED — 单线级联驱动

Hub 使用**单线串行级联** LED（类似 WS2812），通过一根 GPIO 信号线控制多颗级联灯珠，实现高亮远距离可见。

**驱动协议**：

| 项目 | 规格 |
|------|------|
| 驱动方式 | **单线串行级联** (Single-Wire Cascade) |
| 级联接口 | DIN (MCU→灯珠1) → DO (灯珠1→灯珠2) → ... |
| MCU 引脚 | 1 路 GPIO (仅驱动第一颗) |
| 数据帧 | 每灯珠 24bit: **G[7:0] → R[7:0] → B[7:0]**，MSB First |
| 0 码 | T_high=0.20~0.35μs + T_low=0.55~1.2μs |
| 1 码 | T_high=0.55~1.2μs + T_low=0.20~0.35μs |
| RESET 码 | T_low **>80μs** (建议 >100μs) |
| 帧间中断 | 低电平中断 **<35μs**（超过则误判 RESET） |
| 刷新周期 | N×24bit + RESET（例: 12灯珠 ≈ 0.36ms/帧） |
| 支持颜色 | Red, Blue, Yellow, Green, Purple, Orange (RGB 可编程) |
| 亮度要求 | 远距离可见，引导急救人员快速穿过走廊 |
| 低电量 | 亮度值整体降至 50% |

**Zephyr 驱动方案**：使用 `CONFIG_LED_STRIP=y` + 兼容 `worldsemi,ws2812-gpio` 设备树，SPI+DMA 自动生成时序，CPU 零干预。

#### 3.6.2 高分贝蜂鸣器

| 项目 | 规格 |
|------|------|
| 驱动方式 | PWM |
| 频率 | 2-4kHz |
| 音量 | 0-10 级可调 |
| Red 超时 | 60s 自动停止 |
| 其他告警超时 | 30s 自动停止 |
| 低电量 | 音量降至 50% |

#### 3.6.3 LED 颜色规格

| 颜色 | R | G | B | 用途 |
|------|---|---|---|------|
| Red | 255 | 0 | 0 | Code Red |
| Blue | 0 | 100 | 255 | Medical / Secure |
| Yellow | 255 | 220 | 0 | Admin |
| Green | 0 | 255 | 80 | All Clear / Evacuate |
| Purple | 180 | 0 | 255 | Hold |
| Orange | 255 | 120 | 0 | Shelter |

---

### 3.7 按键 ★

仅 1 个复位键：
- 短按：复位设备
- 长按 10s：恢复出厂设置（清除 Config Store）

---

### 3.8 电源管理 ★★★

#### 3.8.1 供电方案

| 方案 | 电池 | 续航 | 适用场景 |
|------|------|------|---------|
| 方案 A | Li-SOCl₂ D 型 19000mAh | 7.8 年+ | 室内安装 |
| 方案 B | 可充电 + 小型太阳能板 | 免维护 | 室外/操场安装 |

#### 3.8.2 功耗计算（19000mAh + 太阳能）

| Ping 周期 | 日均功耗 | 续航 |
|-----------|---------|------|
| 2s (ALERTED) | 6.70 mAh | 7.8 年 |
| 4s (过渡) | 4.15 mAh | >10 年 |
| 8s (NORMAL) | 2.87 mAh | >10 年 |

#### 3.8.3 电源模式

| 组件 | NORMAL | ALERTED | LOW_BATT (<10%) |
|------|--------|---------|------------------|
| Ping Slot | 8s | 2s | 16s |
| BLE 广播 | 2s | 2s | 4s |
| LED 亮度 | 100% | 100% | 50% |
| 蜂鸣器 | 100% | 100% | 50% |
| 心跳 | 5min | 5min | 10min |

---

## 4. 固件架构

### 4.1 线程分配

| 优先级 | 线程 | 触发 | 职责 |
|--------|------|------|------|
| 最高 | lora_thread | sem (DIO1 中断) | LoRa 收发、ping slot、协议解析 |
| 中 | actuator_thread | sem | LED/Buzz 时序控制 |
| 低 | sys_workq | timer | 心跳、电量、BLE 广播管理、GPS、看门狗 |

> Hub 无 ui_thread（无按键/LCD/振动）。

### 4.2 启动流程

```
上电 → MCUboot → HW 初始化 (GPIO/SPI/I2C/ADC/BLE/SX1262/GPS)
→ Config Store → OTAA Join → Class B Beacon 对齐
→ 线程启动 → BLE 广播开始 → GPS 开始定位 → 首次心跳 → 主循环
```

### 4.3 Flash 分区

```
MCUboot(40KB) | Slot-0(460KB) | Slot-1(460KB) | Config(4KB) | Scratch(60KB)
```

---

## 5. 告警数据流（以 Code Red 为例）

```
Dashboard / Badge 触发 Code Red
  → 服务器组播 CMD 0x03 {GroupID=0xFF, Alarm=Red}
  → Hub 在 ping slot 收到
  → GroupID 匹配 (0xFF 命中全部)
  → 告警状态机 → CODE_RED (P0)
  → LED: 红灯闪烁 + 蜂鸣器: 60s
  → HallwayHub 持续亮红直到 All Clear
```

---

## 6. 室外安装要求

| 项目 | 规格 |
|------|------|
| 防水等级 | IP66 / IP67 |
| 温度范围 | 极端温度耐受 |
| 安装位置 | 建筑物外、操场等室外区域 |
| 供电 | 电池 + 太阳能板 |
| 信号 | 需在安装位置正常接收 LoRaWAN 信号 |

---

## 7. 与 Badge 差异

| 特性 | Hub | Badge |
|------|-----|-------|
| BLE 角色 | **Broadcaster** | Scanner |
| GPS | 无 | **有** (RAK12501) |
| LED | 多颗高亮**单线级联** | 2 颗小型 **PWM** |
| 蜂鸣器 | **高分贝** | 小型 |
| 按键 | 1 个复位 | 4 个功能键 |
| LCD | 无 | **有** |
| 振动 | 无 | **有** |
| 供电 | 电池+太阳能 | USB-C 充电 |
| 安装 | 固定安装 | 随身佩戴 |
