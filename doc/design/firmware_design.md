# LoRa 报警系统固件软件框架设计

> 范围：Badge（胸牌）与 Hub（集线器）嵌入式固件，基于 RAK4630 (nRF52840 + SX1262)。
> 网关 RAK7289CV2 为现成设备，服务器端另建，本文仅涉及 Badge/Hub 与网关/服务器的协议交互。

---

## 1. 总体架构

### 1.1 软件分层

```
┌─────────────────────────────────────────────────┐
│                 Application Layer                │
│  ┌──────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │  Alarm   │ │  Button  │ │  Actuator Mgr   │  │
│  │  State   │ │  Event   │ │ (LED/Buzz/Vib/  │  │
│  │  Machine │ │  Proc.   │ │  LCD)           │  │
│  └──────────┘ └──────────┘ └─────────────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │ Protocol │ │   BLE    │ │   Power Mgr     │  │
│  │  Engine  │ │  Locator │ │                 │  │
│  └──────────┘ └──────────┘ └─────────────────┘  │
├─────────────────────────────────────────────────┤
│               Middleware / Service               │
│  ┌──────────┐ ┌──────────┐ ┌─────────────────┐  │
│  │ LoRaWAN  │ │   BLE    │ │  CRC16/XMODEM   │  │
│  │ Class B  │ │  Stack   │ │                 │  │
│  │  Stack   │ │          │ │                 │  │
│  └──────────┘ └──────────┘ └─────────────────┘  │
│  ┌──────────┐ ┌──────────┐                      │
│  │  Flash   │ │  Config  │                      │
│  │  Manager │ │  Store   │                      │
│  └──────────┘ └──────────┘                      │
├─────────────────────────────────────────────────┤
│               Driver Layer (HAL)                 │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌─────┐    │
│  │LED │ │Buzz│ │Vib │ │LCD │ │Btn │ │Batt │    │
│  │PWM │ │PWM │ │GPIO│ │SPI │ │GPIO│ │ ADC │    │
│  └────┘ └────┘ └────┘ └────┘ └────┘ └─────┘    │
│  ┌──────────┐ ┌──────────┐                      │
│  │  SX1262  │ │ External │                      │
│  │  SPI Drv │ │  Flash   │                      │
│  └──────────┘ └──────────┘                      │
├─────────────────────────────────────────────────┤
│          nRF Connect SDK / Zephyr RTOS           │
│  Threads │ Semaphores │ MsgQueues │ Timers       │
│  Power Mgmt │ BLE Controller │ GPIO Interrupts  │
├─────────────────────────────────────────────────┤
│               nRF52840 + SX1262                  │
└─────────────────────────────────────────────────┘
```

### 1.2 线程设计

**Badge 线程分配**（4 线程 + 系统工作队列）：

```
优先级  线程名            周期/触发方式         职责
───────────────────────────────────────────────────
最高    lora_thread        事件驱动(sem)         LoRaWAN 收发、ping slot 守时、协议解析
高      ui_thread          事件驱动(sem)         按键检测、消抖、长短按/组合键识别
中      actuator_thread    事件驱动(sem)         LED/Buzz/Vib/LCD 时序控制（闪烁/间歇）
低      sys_workq          定时器驱动            心跳、电量上报、BLE 扫描触发、看门狗
```

**Hub 线程分配**（3 线程 + 系统工作队列）：

```
优先级  线程名            周期/触发方式         职责
───────────────────────────────────────────────────
最高    lora_thread        事件驱动(sem)         LoRaWAN 收发、ping slot 守时、协议解析
中      actuator_thread    事件驱动(sem)         LED/Buzz 时序控制
低      sys_workq          定时器驱动            心跳、电量上报、BLE 广播管理、看门狗
```

> Hub 无 LCD、无按键（仅复位键）、无振动马达，不需要 ui_thread；BLE 广播由 SoftDevice 自主运行，仅需 sys_workq 更新广播数据。

### 1.3 IPC 机制

```
信号量:
  sem_lora_rx          ← SX1262 DIO1 中断 → lora_thread 唤醒
  sem_lora_tx_done     ← SX1262 DIO1 中断 → lora_thread 等待确认
  sem_button           ← GPIOTE 中断      → ui_thread 唤醒
  sem_actuator_cmd     ← lora_thread/协议  → actuator_thread 执行

消息队列:
  mq_lora_tx           lora_thread 发送队列（协议帧入队）
  mq_lora_rx           lora_thread 接收队列（解析后命令分发）
  mq_actuator          actuator_thread 执行队列（LED/蜂鸣/震动/LCD 命令）
  mq_ble_event         BLE 扫描/广播事件队列

事件标志:
  evt_alarm_change     告警状态变更（通知所有线程）
  evt_power_critical   低电量事件（通知主循环进入省电模式）
```

---

## 2. 重难点问题及解决方案

### 2.1 难点一：Class B 功耗与下行延迟矛盾

**问题**：Code Red 要求 ASAP 全校广播（亚秒级理想），但 Class B ping slot 周期越长越省电，延迟与功耗正相关。

**方案：自适应 Ping Slot 调度**

```
状态机：
  NORMAL  → ping slot = 8s（默认，省电优先）
  ALERTED → ping slot = 2s（收到任意告警后切换，持续 5 分钟）
  TIMEOUT → ping slot = 4s（5 分钟后过渡，再 10 分钟后回 8s）

切换触发：
  1. 设备上行报了 Code Red → 自动切换到 ALERTED 模式
  2. 设备收到下行 Code Red（0x03 红色）→ 切换到 ALERTED 模式
  3. 设备收到 All Clear / Clear All → 切换回 NORMAL 模式

实现关键：
  - 动态调用 LoRaMacSetClassBParam() 修改 ping slot period
  - 在 ALERTED 期设置 5 分钟定时器，超时后梯度恢复
  - Piggyback：在心跳包中上报当前 ping slot 周期，服务器可根据需要远程调整
```

**优化手段**：

| 手段 | 说明 |
|------|------|
| Beacon 预留槽 | Class B beacon 每 128s 一次，ping slot 在其后对齐，避免额外唤醒 |
| 批量下行 | 服务器在同一个 ping slot 窗口内下发多条命令（协议帧支持串联） |
| 多播优先 | Code Red 用 Class B Multicast，一次下行覆盖所有设备，避免逐设备单播 |
| 电池选型 | Hub 建议 Li-SOCl₂ D 型电池 19000mAh，配合小型太阳能板实现免维护 |

**功耗计算（4s ping slot，1000mAh 电池）**：

| Ping 周期 | 日均功耗 | 1000mAh 续航 | Hub（19000mAh+太阳能）续航 |
|-----------|----------|-------------|--------------------------|
| 2s (ALERTED) | 6.70 mAh | 5 个月 | 7.8 年（理论） |
| 4s (过渡) | 4.15 mAh | 8 个月 | >10 年 |
| 8s (NORMAL) | 2.87 mAh | 11.5 个月 | >10 年 |

> 结论：Badge 用可充电电池（常充电），Hub 必须用大容量电池 + 太阳能。

### 2.2 难点二：告警状态机与优先级抢占

**问题**：系统有 8 种告警类型（Red/Blue/Yellow/Green/Hold/Secure/Evacuate/Shelter），多种可能并发激活。设备需正确处理抢占与恢复。

**方案：优先级抢占式告警状态机**

```
优先级定义（数值越小越高）：
  PRIO_CODE_RED    = 0   // 生命安全最高
  PRIO_SHELTER     = 1
  PRIO_EVACUATE    = 2
  PRIO_SECURE      = 3
  PRIO_HOLD        = 4
  PRIO_CODE_BLUE   = 5   // 医疗
  PRIO_CODE_YELLOW = 6   // 管理员
  PRIO_ALL_CLEAR   = 7   // 绿色（仅 Code Red 后可触发）
  PRIO_NORMAL      = 8   // 无告警
```

```
状态转换规则：
  - 高优先级告警 可抢占 低优先级告警
  - 同优先级 不重复触发（已有则忽略）
  - All Clear 只能清除 Code Red（将 Red→Green，Medical/Yellow 如存在则保留）
  - Clear All Statuses 清除全部（→ NORMAL）
  - Green(Code Red中) 的源发出者再次长按 Red → 重新触发 Code Red（新源）

状态存储：
  current_priority  // 当前生效的最高优先级
  active_alarms[]   // 当前激活的告警记录（支持多个低优先级共存）
  每个告警记录: { type, source_room, timestamp, acknowledged }
```

```
示例流程：
  NORMAL
    ← 收到 Code Red          → CODE_RED (P0)，红灯闪烁 + 蜂鸣
    ← 同时收到 Medical       → CODE_RED (P0) + Medical (P5) 共存，优先显示红
    ← 收到 All Clear         → 仅清除 Code Red，Medical 仍在（蓝色）
    ← 收到 Clear All         → NORMAL，全部清除
```

### 2.3 难点三：BLE 定位可靠性

**问题**：RSSI 受多径、人体遮挡影响，仅靠最大 RSSI 选 Hub 可能误判相邻房间。

**方案：多采样排序 + 多 Hub 过滤 + 历史平滑**

```
Badge 报警时的 BLE 扫描流程：

1. 按键确认后 → 启动 4 秒 BLE 扫描（100% duty cycle）
2. 收集所有 Hub 广播包，记录: { Hub_MAC, RSSI[], count }
3. 扫描结束后，每个 Hub 的 RSSI 处理：
   a. 丢弃 count < 2 的 Hub（信号不稳定）
   b. 取 RSSI 中位数（而非均值，抗干扰）
   c. 按中位数 RSSI 降序排列
4. 选择规则：
   a. 若最强的 Hub RSSI 比第二名高 ≥ 6dB → 确定归属该 Hub
   b. 若差值 < 6dB → 取 RSSI 最强，但在协议帧中标记 location_flag=0x01（不确定）
   c. 若所有 Hub RSSI < -85dBm → 标记 location_flag=0x02（弱信号，仅上报 MAC）
5. 上报格式：
   按键事件(0x02) 中填入：最强 Hub MAC + RSSI + location_flag
6. 平滑：
   在"无告警状态"下，Badge 每 30s 被动扫一次 BLE（低功耗），
   记录最近 3 次位置变化，仅当房间变化时才上报，减少上行量。
```

```
Hub 侧 BLE 广播配置：
  - 广播间隔：2s（兼顾定位精度与功耗）
  - 广播数据：AdvA(MAC) + Manufacturing Data(device_type=Hub, room_id)
  - TX Power：+4dBm（nRF52840 内置 PA）
  - 有效距离：20~40m（室内）
```

### 2.4 难点四：按键检测与防误触

**问题**：Badge 有 4 个按键（R/G/B/Y），支持短按（<2s）、长按（3-5s）、组合键（同时 5s）。最关键的是红色 Code Red 长按必须防误触，但又不能太慢。

**方案：分层按键状态机**

```
按键检测流程：

[IDLE]
  │ GPIO 任意按键边沿触发
  ▼
[DEBOUNCE]  (30ms)
  │ 确认按下（否则回 IDLE）
  ▼
[PRESS_TRACK]
  │ 记录按键类型和按下时间
  │ 如果是单键：
  │   ├─ t < 2s 释放 → SHORT_PRESS
  │   ├─ 2s < t < 5s → 进入 LONG_PRESS_PENDING
  │   └─ t > 5s 释放 → LONG_PRESS_CONFIRMED
  │ 如果是双键同时：
  │   └─ t > 5s → COMBO_PRESS
  ▼
[LONG_PRESS_PENDING]  (仅红色按键走此路径)
  │ 到达 3s → LCD 显示确认提示 "Hold 2s to confirm"
  │ LCD 背光亮起，倒计时 2s
  │   ├─ 用户继续按住 → 再等 2s → LONG_PRESS_CONFIRMED（触发 Code Red）
  │   └─ 用户释放 → 取消，LCD 显示 "Cancelled"，然后熄屏
```

```
按键功能映射：

| 按键 | 短按 (<2s)           | 长按 (3-5s)                     | 组合键 (5s)         |
|------|---------------------|---------------------------------|---------------------|
| Red  | 唤醒 LCD+电量显示   | 触发 Code Red（二次确认）       | -                   |
| Blue | 唤醒 LCD+电量显示   | 触发 Medical Alert              | Green+Blue=禁用/启用 |
| Yellow| 唤醒 LCD+电量显示  | 触发 Admin Alert                | Blue+Yellow=复位    |
| Green | 唤醒 LCD+电量显示   | 仅在 Code Red 期间有效：All Clear该房间 | -        |
```

> 关键：Code Red 二次确认机制确保不会因误触而触发全校报警。绿键仅在 Code Red 上下文中有效。

### 2.5 难点五：协议帧解析的鲁棒性

**问题**：LoRa 空口可能丢包、半包、粘包。自定义帧协议需要容错处理。

**方案：流式状态机解析器**

```c
// 帧解析状态机
typedef enum {
    PARSE_IDLE,       // 等待 0xAA
    PARSE_HEAD,       // 收到 0xAA，等待 0x55
    PARSE_HEADER,     // 读 ver + control + cmdid + length(2B) + crc16(2B)
    PARSE_DATA,       // 读 length-9 字节数据
    PARSE_DONE,       // 校验通过，投递到消息队列
    PARSE_ERROR       // 校验失败，丢弃并重置
} parser_state_t;
```

```
容错策略：
  1. 接收缓冲区 512 字节（覆盖最大帧 <256 字节）
  2. 帧头同步：连续扫描 0xAA55，丢失同步后自动重同步
  3. CRC16/XMODEM 校验：校验失败 → 丢弃整帧，不回复 NACK
     （LoRaWAN 层本身有 MIC 校验，应用层 CRC 是额外保护）
  4. 长度校验：length 字段值必须 ≥9（最小帧头长度）且 ≤255
  5. 超时保护：帧解析 5s 内未完成 → 重置状态机
  6. 已知 CMDID 白名单：收到未知 CMDID → 丢弃并记录事件
```

### 2.6 难点六：OTA 固件升级

**问题**：LoRaWAN FUOTA 极慢（数百 bps），电池设备升级中途可能断电。

**方案：MCUboot + 双 Bank + 断点续传**

```
Firmware 分区布局（nRF52840 1MB Flash）:

┌─────────────────── 0x00000000
│  MCUboot (40KB)
├─────────────────── 0x0000A000
│  Slot-0 Primary   (460KB) ← 当前运行固件
├─────────────────── 0x0007D000
│  Slot-1 Secondary (460KB) ← OTA 接收区
├─────────────────── 0x000F0000
│  Config Store     (4KB)   ← 配置参数（GroupID/报警参数等）
├─────────────────── 0x000F1000
│  OTA Scratch      (60KB)  ← 暂存区
└─────────────────── 0x000FFFFF
```

```
FUOTA 流程：
  1. 服务器通过 LoRaWAN FUOTA 规范下发 fragments
  2. 每 fragment 保存到 Slot-1 + CRC32 记录
  3. 全部收完 → CRC32 校验完整镜像
  4. MCUboot 标记 Slot-1 为待测试镜像，重启
  5. MCUboot 引导 Slot-1，新固件运行 N 分钟确认正常
  6. 新固件调用 boot_request_upgrade() 标记永久交换
  7. 若新固件崩溃 → MCUboot 自动回滚到 Slot-0

断点续传：
  - 记录已接收 fragment 的 bitmask 到 Config Store
  - 升级中断后重启 → 上报已接收的 fragment 范围 → 服务器续传
  - 每次收到 fragment 都持久化 bitmask（NVM 磨损均衡处理）
```

### 2.7 难点七：Group ID 角色匹配

**问题**：协议用 bitmask 表示角色（Bit0=admin, Bit1=nurse...），设备需判断自身是否匹配下行的目标组。

**方案**：

```c
// 设备匹配逻辑
bool group_match(uint8_t my_group_id, uint8_t target_group_id) {
    if (target_group_id == 0xFF) return true;   // Bit7=1 表示全部
    if (my_group_id == 0) return false;          // 未经配置的设备不响应
    return (my_group_id & target_group_id) != 0; // 任一位匹配即命中
}

// 场景示例:
// 设备 GroupID = 0b00000011 (admin + nurse)
// target = 0b00000001 (仅 admin)   → match (是 admin)
// target = 0b00000010 (仅 nurse)   → match (是 nurse)
// target = 0b00000100 (仅 secure)  → no match
// target = 0xFF                   → match (广播)
```

> 限制：此设计无法表达"3 楼所有 nurse"（角色 AND 位置），如需此能力需协议扩展。

---

## 3. 核心模块设计

### 3.1 Protocol Engine（协议引擎）

```
proto_engine/
├── proto_parser.c       // 帧解析状态机（上行解析）
├── proto_builder.c      // 帧构建（下行构造）
├── proto_cmd_handler.c  // 命令分发与处理
├── proto_crc16.c        // CRC16/XMODEM
└── proto_validate.c     // 帧校验（长度、CMDID 白名单）
```

**命令处理函数表**：

```c
typedef int (*cmd_handler_t)(const uint8_t *data, uint8_t len);

static const cmd_handler_t cmd_handlers[256] = {
    [0x00] = NULL,              // 上行，不处理
    [0x01] = NULL,              // 上行，不处理
    [0x02] = NULL,              // 上行，不处理
    [0x03] = handle_code,       // 事件码 → Alarm SM
    [0x04] = handle_code_setting, // 事件码参数 → Config Store
    [0x05] = handle_led_control,  // LED 控制（单独控制，不走 Alarm SM）
    [0x06] = handle_buzzer_control,
    [0x07] = handle_vibration_control,
    [0x08] = handle_lcd_content,
    [0x09] = handle_lcd_line2_onoff,
    [0x0A] = handle_clear_packet,
    [0x50] = handle_set_group_id,
};
```

**帧构建示例（按键事件上报）**：

```c
// CMD 0x02 上行：按键事件
int build_key_event(uint8_t *buf, key_event_t *evt) {
    uint8_t *p = buf;
    *p++ = 0xAA; *p++ = 0x55;            // head
    *p++ = 0x01;                          // ver
    *p++ = 0x00;                          // control (请求包)
    *p++ = 0x02;                          // CMDID
    // length 占位，后面回填
    uint8_t *len_ptr = p; p += 2;
    // CRC 占位
    uint8_t *crc_ptr = p; p += 2;
    // data
    *p++ = evt->button;                   // 0=绿 1=蓝 2=黄 3=红
    *p++ = evt->motion;                   // 0=短按 1=长按
    *p++ = evt->rssi;                     // BLE RSSI
    memcpy(p, evt->hub_mac, 6); p += 6;   // Hub MAC
    write_latlon(p, evt->latitude);  p += 4;
    write_latlon(p, evt->longitude); p += 4;

    uint16_t data_len = p - buf;
    *len_ptr++ = (data_len >> 8) & 0xFF;
    *len_ptr   = data_len & 0xFF;

    uint16_t crc = crc16_xmodem(buf, data_len - 2);
    *crc_ptr++ = (crc >> 8) & 0xFF;
    *crc_ptr   = crc & 0xFF;
    return data_len;
}
```

### 3.2 Alarm Manager（告警管理器）

```
alarm_mgr/
├── alarm_sm.c           // 告警状态机核心
├── alarm_priority.c     // 优先级比较与抢占
├── alarm_actuator.c     // 告警 → 执行器命令映射
└── alarm_event_log.c    // 告警事件记录（审计用）
```

**告警 → 执行器映射表**（由 CMD 0x04 配置，此处为默认值）：

```c
typedef struct {
    uint8_t  alarm_type;      // 0~7
    uint8_t  led_color;       // 0=绿 1=蓝 2=黄 3=红 4=紫 5=橙
    uint8_t  led_r, led_g, led_b;
    uint8_t  led_mode;        // 0=常亮 1=闪烁
    uint16_t led_on_ms, led_off_ms;
    uint8_t  buzzer_enable;
    uint8_t  buzzer_mode;
    uint16_t buzzer_on_ms, buzzer_off_ms;
    uint8_t  buzzer_volume;   // 0~10
    uint8_t  vibration_enable;
    uint8_t  vibration_mode;
    uint16_t vib_on_ms, vib_off_ms;
    uint8_t  lcd_line1[20];
    uint8_t  lcd_line2[20];
    uint8_t  lcd_line2_enable;
} alarm_config_t;

// 默认配置（出厂设置，可通过 CMD 0x04 远程修改）
static const alarm_config_t alarm_defaults[8] = {
    [0] = { // Green (All Clear)
        .led_color = 0, .led_r = 0, .led_g = 255, .led_b = 80,
        .led_mode = 1, .led_on_ms = 1000, .led_off_ms = 1000,
        .buzzer_enable = 0, .vibration_enable = 0,
        .lcd_line1 = "All Clear", .lcd_line2_enable = 1,
    },
    [1] = { // Blue (Medical)
        .led_color = 1, .led_r = 0, .led_g = 100, .led_b = 255,
        .led_mode = 0, // 常亮
        .buzzer_enable = 1, .buzzer_mode = 1,
        .buzzer_on_ms = 500, .buzzer_off_ms = 500,
        .buzzer_volume = 5,
        .vibration_enable = 1, .vibration_mode = 1,
        .vib_on_ms = 500, .vib_off_ms = 1500,
        .lcd_line1 = "Medical Alert", .lcd_line2_enable = 1,
    },
    [2] = { // Yellow (Admin)
        // ...
    },
    [3] = { // Red (Code Red)
        .led_color = 3, .led_r = 255, .led_g = 0, .led_b = 0,
        .led_mode = 1, .led_on_ms = 300, .led_off_ms = 300,
        .buzzer_enable = 1, .buzzer_mode = 0, // 常响 60s
        .buzzer_on_ms = 60000, .buzzer_off_ms = 0,
        .buzzer_volume = 10,
        .vibration_enable = 1, .vibration_mode = 1,
        .vib_on_ms = 300, .vib_off_ms = 300,
        .lcd_line1 = "CODE RED", .lcd_line2_enable = 1,
    },
    [4] = { // Hold (Purple, SRP)
        .led_color = 4, .led_r = 180, .led_g = 0, .led_b = 255,
        .led_mode = 1, .led_on_ms = 500, .led_off_ms = 500,
        .buzzer_enable = 1, .lcd_line1 = "Hold Alert",
    },
    [5] = { // Secure (Blue, SRP)
        // ...
    },
    [6] = { // Evacuate (Green, SRP)
        // ...
    },
    [7] = { // Shelter (Orange, SRP)
        // ...
    },
};
```

### 3.3 Actuator Manager（执行器管理器）

```
actuator_mgr/
├── actuator_sm.c        // 执行器总控状态机
├── actuator_led.c       // LED PWM 控制（Duty → 亮度，闪烁定时器）
├── actuator_buzzer.c    // 蜂鸣器控制（PWM 频率 + 音量）
├── actuator_vibration.c // 振动马达控制
├── actuator_lcd.c       // LCD 刷新（SPI 屏，双行 20 字）
└── actuator_utils.c     // 共用的闪烁/间歇定时器
```

**执行器调度策略**：

```
  执行器线程从 mq_actuator 取命令：
    - 高优先级告警的 actuator 命令自动抢占低优先级的
    - LED/Buzz/Vib 可以同时运行（互不冲突）
    - LCD 内容更新与背光独立控制
    - 蜂鸣器自动超时断音（Code Red: 60s, 其他: 30s）
    - 电池 < 10% 时：LED 调暗 50%，蜂鸣器音量减半
```

### 3.4 BLE Manager（蓝牙管理器）

```
ble_mgr/
├── ble_hub_adv.c        // Hub 端：周期性 BLE 广播
├── ble_badge_scan.c     // Badge 端：报警时扫描选 Hub
└── ble_utils.c          // RSSI 处理、MAC 过滤
```

**Hub BLE 广播**：
```
  - 角色：Peripheral Broadcaster
  - 广播数据包（31 字节）：
    [Flags: 0x06] [Complete Local Name: "ALARM_HUB"] [Manufacturer Data: {MAC(6B)}]
  - 间隔：2s
  - TX Power：+4dBm
  - 广播不依赖连接，无需配对
```

**Badge BLE 扫描**：
```
  - 角色：Observer Scanner
  - 触发条件：按键确认后（仅报警时全速扫描）
  - 扫描参数：
    扫描窗口：4s（连续扫描，100% duty cycle）
    PHY：1M (Coded PHY 不需要，距离不是问题）
  - 静默定位（非报警状态）：
    每 30s 被动扫描 1s，仅记录最强 Hub，如房间变化则上报
```

### 3.5 Power Manager（电源管理器）

```
power_mgr/
├── power_state.c        // 电源状态管理
├── power_battery.c      // 电池电压→电量百分比转换
└── power_profile.c      // 各模式功耗预算
```

**电源模式**：

```
┌────────────┬──────────┬──────────┬──────────┬──────────┐
│  组件      │ NORMAL   │ ALERTED  │ LOW_BATT │ CHARGING │
│            │          │ (<10%)   │ (Badge)  │          │
├────────────┼──────────┼──────────┼──────────┼──────────┤
│ Ping Slot  │ 8s       │ 2s       │ 16s      │ 8s       │
│ BLE (Hub)  │ 2s adv   │ 2s adv   │ 4s adv   │ 2s adv   │
│ BLE (Badge)│ 30s 被动 │ 4s 全速  │ 禁用     │ 正常     │
│ LED 亮度   │ 100%     │ 100%     │ 50%      │ 100%     │
│ Buzz 音量  │ 100%     │ 100%     │ 50%      │ 100%     │
│ Vib        │ 100%     │ 100%     │ 禁用     │ 100%     │
│ LCD 背光   │ 按需     │ 常亮     │ 3s 超时  │ 常亮     │
│ 心跳       │ 5min     │ 5min     │ 10min    │ 5min     │
└────────────┴──────────┴──────────┴──────────┴──────────┘
```

**电量上报策略**：

```
  - 变化量 ≥ 5% 且距上次上报 ≥ 5 分钟 → 触发上报
  - 电量 < 30% → 服务器告警（Dashboard 显示 + 短信/邮件）
  - 电量 < 10% → 本地 LED 闪黄灯提醒（Badge）/ 降低功耗模式
```

### 3.6 Button Manager（按键管理器）

```
button_mgr/
├── button_sm.c          // 按键状态机（消抖 + 长短按 + 组合键）
├── button_combo.c       // 组合键识别
└── button_event.c       // 按键事件 → 业务逻辑映射
```

### 3.7 Configuration Manager（配置管理器）

```
config_mgr/
├── config_store.c       // 配置读写（nRF52840 内部 Flash 最后一页）
├── config_defaults.c    // 出厂默认值
└── config_validate.c    // 配置校验（范围检查）
```

**持久化配置项**：

```c
typedef struct {
    // 设备身份
    uint8_t  group_id;          // 角色 bitmask（CMD 0x50 设置）
    uint32_t dev_addr;          // LoRaWAN DevAddr（OTAA 后保存）

    // 告警参数（每类型一套，CMD 0x04 设置）
    alarm_config_t alarm_cfgs[8];

    // Ping Slot 配置
    uint8_t  ping_slot_period;  // 默认 8s

    // BLE 配置
    uint16_t ble_adv_interval_ms;  // Hub 广播间隔，默认 2000
    uint8_t  ble_scan_duration_s;  // Badge 报警扫描时长，默认 4

    // 省电阈值
    uint8_t  low_batt_threshold;   // 默认 10%

    // 看门狗
    uint32_t wdt_timeout_s;        // 默认 120s

    // 校验
    uint32_t crc32;                // 配置块完整性校验
} device_config_t;
```

---

## 4. 核心状态机

### 4.1 告警状态机（Badge）

```
                    ┌─────────────────────────────────────┐
                    │                                     │
                    ▼                                     │
              ┌──────────┐                                │
     ┌───────▶│  NORMAL  │◀────────── Clear All ──────────┤
     │        └─────┬────┘                                │
     │              │                                      │
     │     ┌────────┼────────┐                            │
     │     │        │        │                            │
     │     ▼        ▼        ▼                            │
     │  ┌─────┐ ┌─────┐ ┌──────┐                          │
     │  │ RED │ │BLUE │ │YELLOW│  (Badge 按键触发)        │
     │  └──┬──┘ └──┬──┘ └──┬───┘                          │
     │     │       │       │                               │
     │     │  All Clear     │                               │
     │     │  (仅清除 Red)  │                               │
     │     ▼                │                               │
     │  ┌─────┐             │                               │
     │  │GREEN│ (All Clear) │                               │
     │  └──┬──┘             │                               │
     │     │                │                               │
     │     └─── Green 重按 ─┘─ (重新 Code Red, 新源)       │
     │                │                                    │
     │                ▼                                    │
     │  ┌──────────────────────────────┐                   │
     │  │  SRP 仅由 Dashboard 下发:     │                   │
     │  │  HOLD / SECURE / EVACUATE    │                   │
     │  │  / SHELTER                   │                   │
     │  │  可抢占任意非 Code Red 告警   │                   │
     │  └──────────────────────────────┘                   │
     │                                                     │
     └─────────────────────────────────────────────────────┘
```

### 4.2 按键状态机

```
              ┌──────────────────────┐
              │       IDLE           │◀──── 任意键释放 ────┐
              └──────────┬───────────┘                     │
                         │ GPIO 下降沿中断                  │
                         ▼                                 │
              ┌──────────────────────┐                     │
              │     DEBOUNCE         │ (30ms)              │
              │     等待稳定          │──── 抖动/误触发 ────┤
              └──────────┬───────────┘                     │
                         │ 确认按下                        │
                         ▼                                 │
              ┌──────────────────────┐                     │
              │   PRESS_TRACKING     │                     │
              │   计时 + 识别按键     │                     │
              └──────────┬───────────┘                     │
                         │                                 │
          ┌──────────────┼──────────────┐                  │
          │ t < 2s       │ 2s < t < 5s  │ t > 5s           │
          ▼               ▼              ▼                  │
    ┌──────────┐  ┌──────────────┐ ┌──────────┐           │
    │ 短按处理  │  │ 长按待确认    │ │ 长按确认  │           │
    │          │  │ (RED only:   │ │ 触发报警  │           │
    │ 唤醒LCD  │  │  LCD提示确认) │ │          │           │
    │ 显示电量 │  └──────┬───────┘ └──────────┘           │
    └──────────┘         │ 释放                            │
           │             ▼                                 │
           │      ┌──────────┐                            │
           │      │ CANCEL   │────────────────────────────┘
           │      └──────────┘
           ▼
    ┌──────────┐
    │如果报警   │
    │已激活:    │
    │ 红短按→无│
    │ 绿短按→  │
    │ All Clear│
    │ (仅Code  │
    │  Red中)  │
    └──────────┘
```

---

## 5. 通信协议处理

### 5.1 上行帧构造（Badge/Hub → 服务器）

| CMDID | 函数 | 触发条件 |
|-------|------|----------|
| 0x00 | `build_heartbeat()` | 定时器 5min |
| 0x01 | `build_battery()` | 电量变化 ≥ 5% |
| 0x02 | `build_key_event()` | 按键触发 + BLE 扫描完成 |

### 5.2 下行帧处理（服务器 → Badge/Hub）

| CMDID | 处理函数 | 动作 |
|-------|----------|------|
| 0x03 | `handle_code()` | 检查 GroupID 匹配 → Alarm SM 输入事件码 |
| 0x04 | `handle_code_setting()` | 更新对应告警类型的 alarm_config_t → 保存到 Flash |
| 0x05 | `handle_led_control()` | 直接控制 LED（不走 Alarm SM，用于测试或特殊场景） |
| 0x06 | `handle_buzzer_control()` | 直接控制蜂鸣器 |
| 0x07 | `handle_vibration_control()` | 直接控制振动 |
| 0x08 | `handle_lcd_content()` | 更新 LCD 双行文字 → LCD 刷新 |
| 0x09 | `handle_lcd_line2_onoff()` | LCD 第二行开关 |
| 0x0A | `handle_clear_packet()` | type=0→Alarm SM ClearAll; type=1→Alarm SM AllClear |
| 0x50 | `handle_set_group_id()` | 更新 group_id → 保存到 Flash |

### 5.3 LoRaWAN 端口映射

```
LoRaWAN FPort:
  上行: FPort = 10 (所有上行数据：心跳/电量/按键事件)
  下行: FPort = 10 (所有下行命令：单播)
        FPort = 20 (多播命令：Code Red / SRP 广播)
```

> 多播使用独立的 Multicast Group，设备入网后由服务器下发 Multicast Session（McAddr + McNwkSKey + McAppSKey）。

---

## 6. 启动与初始化流程

```
上电 / 复位
  │
  ▼
MCUboot 检查 Slot-1 是否有待确认固件
  │
  ├─ 有待确认 → 启动 Slot-1，运行 N 分钟后自动确认
  └─ 无       → 启动 Slot-0
  │
  ▼
硬件初始化
  ├─ GPIO 初始化（LED/Buzz/Vib/LCD/按键）
  ├─ SPI 初始化（SX1262 + LCD）
  ├─ ADC 初始化（电池电压）
  ├─ BLE SoftDevice 初始化
  └─ 读取 Config Store（group_id / alarm_cfgs / ping_slot）
  │
  ▼
LoRaWAN 入网 (OTAA) — 无限重试 + 指数退避
  ├─ BLE 先行启动 (入网前即开始广播/扫描，离线可用)
  ├─ 检查已保存的 Session Context
  │   ├─ 有效 → 尝试恢复 Session
  │   └─ 无效 → 发起 Join Request
  ├─ 入网失败 → 指数退避重试
  │   ┌──────────┬──────────┐
  │   │ 尝试次数  │ 退避间隔  │
  │   ├──────────┼──────────┤
  │   │    1     │   10s    │
  │   │    2     │   20s    │
  │   │    3     │   40s    │
  │   │    4     │   80s    │
  │   │    5     │  160s    │
  │   │   6+     │  900s (15min) │
  │   └──────────┴──────────┘
  ├─ 连续失败 ≥6 次 → OLED 显示 "Join Failed!" + LED 红快闪
  ├─ 入网期间 OLED 显示 "LoRa Joining..."/"Retry in XXs"
  ├─ Join Accept → 保存 DevAddr/NwkSKey/AppSKey
  └─ 入网成功后 → 启动 Class B (Beacon 搜索 → Ping Slot 对齐)
  │
  ▼
线程启动
  ├─ lora_thread      (PRIO_HIGH)
  ├─ ui_thread        (PRIO_HIGH, Badge only)
  ├─ actuator_thread  (PRIO_NORMAL)
  └─ sys_workq        (PRIO_LOW)
  │
  ▼
首次心跳上报 (CMD 0x00)
  │
  ▼
进入主循环（休眠 + 事件驱动唤醒）
```

---

## 7. 关键数据流

### 7.1 Code Red 报警完整数据流

```
Badge                                    Gateway/LNS/Server
  │                                           │
  ├─ 用户长按 Red 键 3s                        │
  ├─ LCD: "Hold 2s to confirm"                │
  ├─ 用户继续按住 2s                            │
  ├─ LCD: "CODE RED - SENDING..."             │
  │                                           │
  ├─ 启动 BLE 扫描 4s                          │
  ├─ 收集 Hub RSSI → 选最强 Hub MAC            │
  │                                           │
  ├─ LoRa TX: CMD 0x02 ──────────────────────▶│
  │   {button=Red, long_press, rssi, hub_mac} │
  │                                           ├─ Server 收到 Code Red 源
  │                                           ├─ 查询该 Hub 的房间号
  │                                           ├─ 组播下行: CMD 0x03
  │   ◀───────────────────────────────        │   {GroupID=0xFF, Alarm=Red}
  │   (在下一个 ping slot 收到)                │
  │                                           │
  ├─ AlarmSM: 切换到 CODE_RED (P0)             │
  ├─ Actuator: 红灯闪烁 300ms                   │
  ├─ Actuator: 蜂鸣器 60s                       │
  ├─ Actuator: 振动马达间歇                     │
  ├─ LCD: "CODE RED" (第一行)                  │
  │                                           │
  │  ... (等待 All Clear / Clear All)         │
  │                                           │
  │   ◀── CMD 0x0A {type=1(All Clear)} ──────│
  ├─ AlarmSM: 切换到 ALL_CLEAR (P7)            │
  ├─ Actuator: 绿灯闪烁                         │
  └─ Actuator: LCD "All Clear"                 │
```

### 7.2 Hub LED 告警显示流程

```
Hub
  │
  │  (在 ping slot 收到下行)                    │
  │   ◀── CMD 0x03 {GroupID=0xFF, Alarm=Red} ─│
  │                                           │
  ├─ GroupID 匹配 (0xFF 全部)                  │
  ├─ AlarmSM: 切换到 CODE_RED (P0)             │
  ├─ 根据 Hub 类型决定效果:                     │
  │   RoomHub:   红灯闪烁                       │
  │   DoorHub:   红灯闪烁 (可根据来源房间 + BLE  │
  │              进一步区分颜色)                 │
  │   HallwayHub:红灯闪烁 (持续到 All Clear)     │
  │                                           │
  ├─ Actuator: LED PWM → 高亮红色闪烁           │
  └─ Actuator: 蜂鸣器 60s 后自动停止             │
```

---

## 8. 目录结构

```
firmware/
├── CMakeLists.txt
├── prj.conf                      # Zephyr 内核配置
├── Kconfig                       # 应用层配置选项
│
├── boards/
│   └── arm/rak4630/
│       ├── board.cmake
│       ├── rak4630_badge.dts     # Badge 设备树
│       ├── rak4630_hub.dts       # Hub 设备树
│       └── pinmux.c
│
├── src/
│   ├── main.c                    # 入口 + 初始化
│   │
│   ├── app/
│   │   ├── alarm_sm.c            # 告警状态机
│   │   ├── alarm_sm.h
│   │   ├── actuator_mgr.c        # 执行器管理器
│   │   ├── actuator_mgr.h
│   │   ├── power_mgr.c           # 电源管理器
│   │   └── power_mgr.h
│   │
│   ├── proto/
│   │   ├── proto_parser.c        # 帧解析
│   │   ├── proto_builder.c       # 帧构建
│   │   ├── proto_handler.c       # 命令分发
│   │   ├── proto_crc16.c         # CRC16/XMODEM
│   │   └── proto_internal.h
│   │
│   ├── ble/
│   │   ├── ble_hub_adv.c         # Hub BLE 广播
│   │   ├── ble_badge_scan.c      # Badge BLE 扫描
│   │   ├── ble_locator.c         # 定位算法
│   │   └── ble_internal.h
│   │
│   ├── ui/
│   │   ├── button_sm.c           # 按键状态机 (Badge only)
│   │   ├── button_combo.c        # 组合键
│   │   ├── lcd_drv.c             # LCD 驱动 (Badge only)
│   │   └── ui_internal.h
│   │
│   ├── drv/
│   │   ├── led_pwm.c             # LED PWM 驱动
│   │   ├── buzzer_pwm.c          # 蜂鸣器 PWM 驱动
│   │   ├── vibration_gpio.c      # 振动马达 GPIO
│   │   ├── battery_adc.c         # 电池 ADC 采样
│   │   └── drv_internal.h
│   │
│   ├── hal/
│   │   ├── hal_sx1262.c          # SX1262 SPI 封装
│   │   ├── hal_flash.c           # Flash 读写封装
│   │   └── hal_internal.h
│   │
│   ├── config/
│   │   ├── config_store.c        # 配置持久化
│   │   ├── config_defaults.c     # 出厂默认值
│   │   └── config_validate.c
│   │
│   └── utils/
│       ├── crc16.c               # CRC16/XMODEM
│       ├── ringbuf.c             # 环形缓冲区
│       └── utils.h
│
├── tests/
│   ├── unit/
│   │   ├── test_alarm_sm.c
│   │   ├── test_proto_parser.c
│   │   ├── test_proto_crc16.c
│   │   ├── test_button_sm.c
│   │   └── test_group_match.c
│   └── integration/
│       ├── test_rx_tx_loop.c     # LoRa 自发自收测试
│       └── test_ble_locator.c
│
└── doc/
    └── firmware_design.md
```

---

## 9. 编译与构建

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_alarm_firmware)

# 根据 BOARD 区分 Badge / Hub
if(CONFIG_BOARD_RAK4630_BADGE)
    set(DEVICE_TYPE "badge")
elseif(CONFIG_BOARD_RAK4630_HUB)
    set(DEVICE_TYPE "hub")
endif()

target_compile_definitions(app PRIVATE DEVICE_TYPE_${DEVICE_TYPE})

# 源文件
target_sources(app PRIVATE
    src/main.c
    src/app/alarm_sm.c
    src/app/actuator_mgr.c
    src/app/power_mgr.c
    # ...
)

# Badge 独有源文件
if(DEVICE_TYPE STREQUAL "badge")
    target_sources(app PRIVATE
        src/ui/button_sm.c
        src/ui/button_combo.c
        src/ble/ble_badge_scan.c
    )
else()
    target_sources(app PRIVATE
        src/ble/ble_hub_adv.c
    )
endif()
```

**构建命令**：

```bash
# Badge 固件
west build -b rak4630_badge -d build/badge

# Hub 固件
west build -b rak4630_hub -d build/hub

# 单元测试（在 host 上运行）
west build -b native_posix -d build/test -- -DCONFIG_TEST=y
```

---

## 10. 与硬件设计的关键交互

| 软件需求 | 硬件要求 | 备注 |
|----------|----------|------|
| 电池电压采样 | ADC 引脚接电池分压（1:1 或 1:2） | 精度 ±5% 即可 |
| LED RGB 控制 | 3 路 PWM（R/G/B 独立通道） | Hub 需要高亮 LED 驱动（MOSFET） |
| 蜂鸣器 | 1 路 PWM | 频率 2-4kHz，带 MOS 管驱动 |
| 振动马达 | 1 路 GPIO | 带 MOS 管驱动 |
| 按键 (Badge) | 4 路 GPIO + 中断 | 需要外接上拉电阻 |
| LCD (Badge) | SPI 或 I2C 接口 | 推荐小尺寸 OLED/STN 屏 |
| 充电检测 (Badge) | 1 路 GPIO 或 ADC | 检测 USB-C 插入 |
| BLE 天线 | nRF52840 内置 | 与 LoRa 天线分开放置（频率不同） |
| OTA 存储 | 外接 SPI Flash ≥ 512KB | nRF52840 内部 1MB 足够 |
| 看门狗 | 使用 nRF52840 内部 WDT | 120s 超时 |

---

## 11. 测试策略

### 单元测试（native_posix 平台）

- `test_alarm_sm.c`：告警状态机所有转换路径
- `test_proto_parser.c`：帧解析（正常帧 + 异常帧 + CRC 错误 + 截断帧）
- `test_proto_crc16.c`：CRC16/XMODEM 已知向量验证
- `test_button_sm.c`：按键消抖 + 长按/短按/组合键
- `test_group_match.c`：GroupID bitmask 匹配各种组合

### 集成测试（RAK4630 硬件）

- 自发自收测试（LoRa TX → RX 完整链路）
- BLE 定位测试（不同距离、不同遮挡条件）
- 功耗测试（各模式下实际电流测量）
- 72 小时运行稳定性（NORMAL 模式下持续运行）

### 压力测试

- 连续 100 次报警触发 → 验证状态机不会卡死
- 快速切换告警类型 → 验证抢占逻辑
- 断电恢复 → 验证配置持久化和 Session 恢复

---

## 附录 A：Badge 与 Hub 差异对照表

| 特性 | Badge | Hub |
|------|-------|-----|
| 按键 | 4 个 (R/G/B/Y) | 1 个复位键 |
| LED | 2 颗 RGB LED | 多颗高亮 RGB LED（外接 MOS 驱动） |
| 蜂鸣器 | 小型蜂鸣器 | 高分贝蜂鸣器 |
| 振动 | 有 | 无 |
| LCD | 双行 20 字 LCD | 无 |
| BLE | Observer Scanner | Broadcaster |
| 供电 | 可充电电池 (USB-C) | 一次性/可充电 + 太阳能 |
| LoRaWAN | Class B | Class B |
| 定位 | 被定位（扫描 Hub） | 提供定位基准（广播） |

## 附录 B：配置参数默认值汇总

| 参数 | 默认值 | 可配置 | 说明 |
|------|--------|--------|------|
| Ping Slot 周期 | 8s | Y (CMD 0x04) | NORMAL 模式下的 ping 间隔 |
| 心跳周期 | 300s | Y | 5 分钟 |
| BLE 广播间隔 (Hub) | 2s | Y | |
| BLE 扫描窗口 (Badge) | 4s | Y | 报警定位扫描时长 |
| 蜂鸣器超时 | 60s (Red) / 30s (其他) | Y (CMD 0x04) | 自动停止蜂鸣 |
| 按键消抖 | 30ms | N | |
| 长按阈值 | 3s (Red 二次确认) | N | 其余按键直接触发 |
| 低电量阈值 | 10% | Y | |
| 看门狗超时 | 120s | N | |
| Code Red 闪烁周期 | 300ms on / 300ms off | Y (CMD 0x04) | |
