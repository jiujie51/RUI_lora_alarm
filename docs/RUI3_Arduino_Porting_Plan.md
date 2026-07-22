# NCS LoRa Alarm → RUI3/Arduino 移植方案

## 背景

将 `ncs_lora_alarm/`（基于 Zephyr/NCS，5782 行，48 个文件）完整移植到 RUI3（RAK 统一接口 V3，兼容 Arduino）平台，目标目录 `arduino_lora_alarm/`。

硬件：RAK4630（nRF52840 + SX1262），两种设备固件 — Badge（胸牌）和 Hub（集线器）。

---

## 可行性结论：**可行，约 10 个工作日**

**移植工作量分解：**
| 类别 | 行数 | 占比 | 说明 |
|------|------|------|------|
| 直接复制 | ~3800 行 | 57% | 纯 C 业务逻辑，仅替换 `k_uptime_get()`→`millis()` 和日志宏 |
| 驱动重写 | ~1500 行 | 22% | GPIO/PWM/ADC/I2C/UART/Flash → Arduino API |
| 架构重写 | ~1400 行 | 21% | 线程/信号量/定时器 → `loop()` + `millis()`；LoRaWAN/BLE API 全部替换 |

---

## RUI3 示例文件对照表

每个 NCS 功能模块对应的 RUI3 官方示例：

| NCS 功能 | RUI3 示例文件 | 演示的核心 API |
|----------|-------------|---------------|
| **LoRaWAN OTAA 入网** | `Example/LoRaWan_OTAA/LoRaWan_OTAA.ino` | `api.lorawan.njm.set(RAK_LORA_OTAA)`, `join()`, `njs.get()`, `registerRecvCallback()`, `registerJoinCallback()` |
| **LoRaWAN Class B** | `Example/LoRaWan_Class_B/LoRaWan_Class_B.ino` | `api.lorawan.deviceClass.set(RAK_LORA_CLASS_B)` — 内置 Beacon 锁定、Ping Slot 管理 |
| **LoRaWAN 多播** | `Example/LoRaWan_Multicast/LoRaWan_Multicast.ino` | `RAK_LORA_McSession`, `api.lorawan.addmulc(session)` |
| **LoRaWAN 上行/下行** | `Example/LoRaWan_OTAA/LoRaWan_OTAA.ino` | `api.lorawan.send(len, buf, port, confirm, retry)`; `SERVICE_LORA_RECEIVE_T` 回调 |
| **BLE 扫描** | `Example/BLE_Scanner/BLE_Scanner.ino` | `api.ble.scanner.start(0)`, `setInterval()`, `setScannerCallback(scan_callback)` |
| **BLE 广播（Hub）** | `Example/BLE_Beacon_Custom_Payload/BLE_Beacon_Custom_Payload.ino` | `api.ble.beacon.custom.payload.set(data, len)` — Hub 用自定义 Manufacturer Data（MAC + device_type + room_id），非 iBeacon 格式 |
| **系统定时器** | `Example/System_Timer/System_Timer.ino` | `api.system.timer.create(RAK_TIMER_0, handler, RAK_TIMER_PERIODIC)`, `start()` — 回调在事件循环中执行（非 ISR） |
| **协作式多线程** | `Example/RAK_Thread/RAK_Thread.ino` | `RT_BEGIN(rt)`, `RT_SLEEP(rt, ms)`, `RT_YIELD(rt)`, `RT_SCHEDULE(...)` — Protothreads |
| **GPIO 中断 / 按键** | `Example/Arduino_Interrupt/Arduino_Interrupt.ino` | `attachInterrupt(pin, ISR, FALLING)`, `volatile` 标志共享 |
| **LED PWM / 呼吸灯** | `Example/Arduino_Led_Breathing/Arduino_Led_Breathing.ino` | `analogWrite(pin, val)` — 0-255 占空比 |
| **串口 / UART** | `Example/Arduino_Serial/Arduino_Serial.ino` | `Serial.begin(baud, RAK_CUSTOM_MODE)`, `Serial1` 第二路串口 |
| **电池 ADC** | `Example/System_General/System_General.ino` | `api.system.bat.get()` — 返回 float 电压值 |
| **Flash 存储** | `Application_Scenario/LoRa/RUI3-Power-Test/custom_at.cpp` | `api.system.flash.get(offset, buf, len)`, `api.system.flash.set(...)` |
| **低功耗 / 睡眠** | `Example/System_Powersave/System_Powersave.ino` | `api.system.sleep.all(ms)`, `api.system.sleep.setup()`, `registerWakeupCallback()` |
| **自定义 AT 命令** | `Example/System_Custom_ATCMD/System_Custom_ATCMD.ino` | `api.system.atMode.add("CMD", "desc", "CMD", handler)` |
| **RAK4631 基础** | `Example/RAK4631/RAK4631.ino` | BLE UART 初始化，`api.system.scheduler.task.destroy()` 用于低功耗 |
| **数字 GPIO** | `Example/Arduino_Digital/Arduino_Digital.ino` | `pinMode(pin, INPUT_PULLUP)`, `digitalRead()`, `digitalWrite()` |
| **传感器节点应用** | `Application_Scenario/LoRa/RUI3-Sensor-Node/` | 完整应用参考：37 种 I2C 传感器自动识别、Cayenne LPP 编码、定时发送 |

---

## 关键问题的解答

### 1. OLED 显示（SSD1306）— **可行，使用硬件 I2C（TWI1）**

**问题**：Badge OLED 使用 P0.29 (SDA) / P0.30 (SCL)，这两个是 QSPI 引脚，**不是** RUI3 默认的 Wire 总线（P0.13/P0.14）。

**方案**：使用 nRF52840 的**第二个 TWI 外设（TWI1）**，配置为 P0.29/P0.30。

- nRF52840 有 2 个 TWI（I2C）实例：TWI0 已被 `Wire` 占用（P0.13/P0.14），TWI1 空闲
- RAK4631 使用 SPI Flash（不用 QSPI），因此 P0.29/P0.30 是空闲 GPIO，可自由复用
- TWI 外设通过 PSEL 寄存器支持任意 GPIO 引脚映射
- RUI3 底层 `I2cMcuInit(obj, i2cId, scl, sda)` 接受引脚参数，直接传入 `P0_29`/`P0_30` 即可

**实现方式**：
```cpp
// 初始化 TWI1 在 P0.29 (SDA) / P0.30 (SCL)
I2c_t oled_i2c;
I2cMcuInit(&oled_i2c, I2C_1, P0_29, P0_30);        // I2C_1 = TWI1
I2cMcuFormat(&oled_i2c, MODE_I2C, I2C_DUTY_CYCLE_2,
             true, I2C_ACK_ADD_7_BIT, 400000);       // 400kHz
```
- `ncs_lora_alarm/drv/oled_drv.c` 中的 450+ 行字体数据（6x8 + 8x16 ASCII + 16x16 中文）和所有绘图函数是纯 C，直接复制
- 只需将 `oled_i2c_write()` 下的 Zephyr I2C 调用替换为 `I2cMcuWriteBuffer()` / `I2cMcuReadBuffer()`

**OLED 引脚映射：**
| 功能 | 引脚 | Arduino API |
|------|------|-------------|
| OLED SDA | P0.29 | 硬件 I2C（TWI1, `I2cMcuInit` 配置） |
| OLED SCL | P0.30 | 硬件 I2C（TWI1） |
| OLED PWR | P0.26 | `digitalWrite(P0_26, HIGH)` |
| OLED RST | P0.28 | `digitalWrite(P0_28, ...)` |

### 2. 业务逻辑实现 — **用 RUI3 Protothreads 实现协作式多任务**

**需要 actuator_thread 类似的独立执行单元。** NCS 原版使用两条线程：

1. **lora_thread**（高优先级）：阻塞等待定时器唤醒 → 发送心跳/电量上报
2. **actuator_thread**（中优先级）：每 10ms 调用 `actuator_mgr_tick()` 驱动 LED/蜂鸣器/振动模式

在 RUI3 中，使用 **Protothreads**（参考 `Example/RAK_Thread/RAK_Thread.ino`）实现等效的并发模型：

```
NCS 多线程模型                       RUI3 Protothreads 模型
─────────────────────────────       ─────────────────────────
lora_thread (优先级2, 阻塞等待)  →  loraThread 协程：RT_YIELD 轮询 flag
actuator_thread (10ms tick)     →  actuatorThread 协程：RT_SLEEP(rt, 10)
k_timer (4个, ISR上下文)         →  api.system.timer + volatile bool flag
k_sem_give/k_sem_take             →  flag + 协程间轮询
```

**定时器 → loraThread 通知机制（核心 IPC）：**

```
api.system.timer 回调                  loraThread 协程
(事件队列上下文, 非 ISR!)              (loop() 中由 RT_SCHEDULE 调度)
─────────────────────────             ───────────────────────────
g_heartbeat_pending = true;     →    if (g_heartbeat_pending) {
g_power_report_pending = true;  →        send_heartbeat();  // ✅ 在 loop() 上下文中调用
                                         api.lorawan.send(...);
                                     }
```

关键设计：
- `api.system.timer` 回调**不是 ISR**（`System_Timer.ino` 注释："handler is not executed in interrupt context"），它只是往事件队列发消息，由 `loop()` 中的 `rui_running()` 调度执行
- 定时器回调**只设置 `volatile bool` flag**，不调用 `send()`
- `loraThread` 协程每次 `RT_YIELD` 后检查 flag，发现为真就调用 `send_heartbeat()` → `api.lorawan.send()` — **此时在 `loop()` 上下文中，安全**
- 与 NCS 原版完全等价：`k_sem_give(ISR)` → `k_sem_take` 唤醒线程 → 线程调用 `lorawan_send()`

**loraThread 实现：**
```cpp
volatile bool g_heartbeat_pending = false;
volatile bool g_power_report_pending = false;

// 定时器回调（事件队列上下文）
void heartbeat_timer_cb(void *) {
    if (api.lorawan.njs.get()) g_heartbeat_pending = true;
}
void power_report_timer_cb(void *) {
    if (api.lorawan.njs.get()) g_power_report_pending = true;
}

// loraThread 协程
int loraThread(struct rt *rt) {
    RT_BEGIN(rt);
    for (;;) {
        RT_YIELD(rt);  // 让出 CPU，给 actuator/button/UI 执行机会

        if (g_heartbeat_pending) {
            g_heartbeat_pending = false;
            send_heartbeat();  // → proto_build_heartbeat → app_hal_send
        }
        if (g_power_report_pending) {
            g_power_report_pending = false;
            send_power_report();
        }
    }
    RT_END(rt);
}
```

**`loop()` 中调度多个 Protothreads：**
```cpp
rt rtLora, rtActuator, rtButton, rtBadgeUi;

void loop() {
    RT_SCHEDULE(loraThread(&rtLora));          // 心跳/电量 TX（flag 驱动）
    RT_SCHEDULE(actuatorThread(&rtActuator));  // 10ms: LED/蜂鸣器/振动 tick
    RT_SCHEDULE(buttonThread(&rtButton));      // 按键消抖 + 长按检测
    RT_SCHEDULE(badgeUiThread(&rtBadgeUi));    // OLED 刷新 + 确认状态机
}
```

**Protothreads 优势：**
- 同时满足多个周期性任务的时间约束（10ms 执行器 tick、30ms 按键消抖、100ms OLED 刷新）
- `RT_SLEEP(rt, ms)` 比 `delay()` 更精确——不阻塞其他任务
- 代码结构与 NCS 原版线程模型一一对应，移植时语义一致

### 3. LoRaWAN 配置 — **RUI3 的 `api.lorawan.*` 完整覆盖**

**功能映射：**
| NCS (Zephyr lorawan) | RUI3 API |
|----------------------|----------|
| OTAA 入网，指数退避重试 | `api.lorawan.njm.set(RAK_LORA_OTAA)` + `api.lorawan.join()` + 非阻塞 join_callback |
| 上行发送 (send) | `api.lorawan.send(len, data, fport, confirm, retry)` |
| 下行接收 (registerRecvCallback) | `api.lorawan.registerRecvCallback(cb)` |
| Class B | `api.lorawan.deviceClass.set(RAK_LORA_CLASS_B)` **内置支持** — 删除整个 `lorawan_classb.c` |
| 多播 (4 groups) | `api.lorawan.addmulc(session)` — 替代 `lorawan_mc.c` 中的 `LoRaMacMcChannelSetup` |
| ADR, 信道掩码 | `api.lorawan.adr.set()`, `api.lorawan.mask.set()` |
| 凭证 (DevEUI/JoinEUI/AppKey) | `api.lorawan.deui.set()` / `.appeui.set()` / `.appkey.set()` |

**关键注意事项：**
- `api.lorawan.send()` 是阻塞的 — **绝对不能**在定时器回调中调用，必须在 `loop()` 中调用
- RUI3 的 LoRaWAN 栈使用相同的 loramac-node 底层，空中行为完全一致
- RUI3 的 NVM 会自动保存 DevNonce、帧计数器、会话密钥，不需要应用层干预

### 4. 硬件引脚配置 — **直接映射，需注意几个冲突**

**RUI3 硬件引脚配置文件位置：**

| 文件 | 用途 |
|------|------|
| `RUI3/variants/WisCore_RAK4631_Board/variant.h` | **主引脚映射文件** — Arduino 引脚号→ nRF52 GPIO 映射、`digitalPinHasPWM`、引脚别名（`LED_GREEN`, `WB_IO1`, `PIN_A0` 等） |
| `RUI3/variants/WisCore_RAK4631_Board/variant.cpp` | **`g_ADigitalPinMap[]` 数组的实现** — Arduino 引脚 N → nRF52 GPIO N（直接映射，P0=0..31, P1=32..47） |
| `RUI3/variants/WisCore_RAK4631_Board/pin_define.h` | **外设引脚分配** — `I2C0_SDA`/`I2C1_SCL`, `SPIM3_MOSI`/`SPIM3_MISO`, `UART0_TXD`/`UART1_RXD`, `GREEN_LED`/`BLUE_LED` |
| `RUI3/variants/WisCore_RAK4631_Board/board-config.h` | **LoRa 射频引脚** — `RADIO_MOSI(P1.12)`, `RADIO_MISO(P1.13)`, `RADIO_SCLK(P1.11)`, `RADIO_NSS(P1.10)`, `RADIO_DIO_1(P1.15)` |
| `RUI3/variants/WisCore_RAK4631_Board/pins_arduino.h` | **Arduino 兼容层** — 仅 `#include "variant.h"` |
| `RUI3/cores/nRF5/component/rui_v3_api/wiringDigital.cpp` | **`pinMode()`/`digitalWrite()`/`digitalRead()` 的实现** |
| `RUI3/cores/nRF5/component/rui_v3_api/wiringTone.cpp` | **`analogWrite()` 的实现** — 490Hz PWM，最多 3 路同时（`UDRV_PWM_MAX=3`） |
| `RUI3/cores/nRF5/component/core/mcu/nrf52840/uhal/uhal_adc.c` | **ADC 通道映射** — `get_nrf_adc_pin()`: P0.02→AIN0, P0.05→AIN3, P0.31→AIN7 等 |
| `RUI3/boards.txt` | **编译配置** — `build.f_cpu=64000000`, `build.extra_flags=-DNRF52840_XXAA -Drak4630 -DWISBLOCK_BASE_5005_O` |

**关键发现：**
- `digitalPinHasPWM(P)` 定义为 `(g_ADigitalPinMap[P] > 1)` — **所有引脚（除 P0.00/P0.01 晶振外）都支持 PWM**，包括 P0.03（QSPI_CLK）
- ⚡ **`analogWrite` 只能同时 3 路**（`UDRV_PWM_MAX=3`，使用 `app_pwm`(TIMER1/2/3) + GPIOTE 翻转，单通道、固定 490Hz）。但 nRF52840 另有 **4 个硬件 PWM 外设（NRF_PWM0/1/2/3）未被 RUI3 使用**，每实例支持 4 路独立输出
- Badge 5 路 PWM 方案：**3 路 `analogWrite`（RGB LED）+ 2 路 `nrfx_pwm`（蜂鸣器 3kHz + 振动 20kHz）**，参考 NCS overlay 引脚配置（`NRF_PWM2→P0.13`, `NRF_PWM3→P0.14`）
- `Wire`（I2C）默认使用 `WB_I2C1_SDA(P0.13)/SCL(P0.14)`。OLED 通过 **TWI1 硬件 I2C** 驱动 P0.29/P0.30（`I2cMcuInit(&obj, I2C_1, P0_29, P0_30)`），不是软件 I2C
- ADC 为 14 位分辨率，电池测量推荐直接用 `api.system.bat.get()`

#### Flash 布局、config_store 与 RUI3 NVM 职责划分

**NCS 与 RUI3 Flash 布局完全不同：**

```
NCS (Zephyr)                          RUI3 (RAK4630, nRF52840 1MB)
─────────                             ─────────
                                      0x00000 ┌──────────────┐
                                              │ MBR+SoftDevice│
                                      0x26000 ├──────────────┤
                                              │ RUI3 App      │ ~300KB
                                      0x70000 ├──────────────┤
                                              │ 用户 Flash    │ 128KB ← api.system.flash
                                      0x90000 ├──────────────┤
                                              │ FDS / NVM     │ RUI3 LoRaWAN 自动化
                                      0xFC000 ├──────────────┤ ← NCS 想用这里 (16KB)
0xFC000 ┌── Primary  ────┐                   │ ???           │
0xFD000 ├── Backup1  ────┤  NCS 最后16KB     │               │
0xFE000 ├── Backup2  ────┤                   │               │
0xFF000 └── Factory  ────┘                   └──────────────┘
```

**config_store 偏移重映射到 RUI3 用户 Flash：**
```cpp
// 相对偏移（api.system.flash 内部加 0x70000 基址）
#define CONFIG_PRIMARY_OFFSET   0x00000    // 绝对地址 0x70000
#define CONFIG_BACKUP1_OFFSET   0x01000    // 绝对地址 0x71000
#define CONFIG_BACKUP2_OFFSET   0x02000    // 绝对地址 0x72000
#define CONFIG_FACTORY_OFFSET   0x03000    // 绝对地址 0x73000
```

**RUI3 自动管理的 LoRaWAN 数据（应用层不需要存）：**

当调用 `api.lorawan.xxx.set()` 时，RUI3 内部自动调用 `service_nvm_set_xxx_to_nvm()` 写入 FDS Flash，断电不丢失：

| RUI3 API | 自动持久化内容 |
|----------|-------------|
| `api.lorawan.deui.set()` | DevEUI |
| `api.lorawan.appeui.set()` | JoinEUI |
| `api.lorawan.appkey.set()` | AppKey |
| `api.lorawan.njm.set()` | 入网模式 (OTAA/ABP) |
| `api.lorawan.band.set()` | 频段 |
| `api.lorawan.deviceClass.set()` | Class A/B/C |
| `api.lorawan.adr.set()` | ADR 开关 |
| **OTAA 入网成功后（SDK 自动）** | DevAddr, NwkSKey, AppSKey, FCnt, DevNonce |
| `api.lorawan.addmulc()` | 多播会话密钥 |

**移植版 config_store 只需存储 4 个应用层字段（共 4 字节）：**

| 字段 | 说明 | RUI3 自动管理？ |
|------|------|:--:|
| `group_id` | 角色 bitmask（CMD 0x50 远程设置） | ❌ |
| `device_type` | Badge=0 / Hub=1 | ❌ |
| `hub_type` | RoomHub=0 / DoorHub=1 / HallwayHub=2 | ❌ |
| `room_id` | 房间号（CMD 0x50 远程设置） | ❌ |
| ~~`dev_nonce`~~ | OTAA 防重放 | ✅ RUI3 自动 |
| ~~`dev_eui`~~ | 设备 EUI | ✅ `api.lorawan.deui.set()` |
| ~~`join_eui`~~ | 应用 EUI | ✅ `api.lorawan.appeui.set()` |
| ~~`app_key`~~ | 应用密钥 | ✅ `api.lorawan.appkey.set()` |

- 总占用 16KB（4 槽 × 4KB），远小于 128KB 上限
- 完全保留 NCS 原版机制：魔数 `0x4C4F5241` → CRC32 校验 → 多槽恢复 → 轮流写入磨损均衡
- 写入用 `api.system.flash.set()`，RUI3 内部自动加 `0x70000` 基址

**Badge 引脚映射表：**
| 功能 | NCS 引脚 | RUI3 引脚名 | Arduino API |
|------|---------|-------------|-------------|
| OLED SDA | P0.29 | P0_29 | 硬件 I2C（TWI1, `I2cMcuInit`） |
| OLED SCL | P0.30 | P0_30 | 硬件 I2C（TWI1） |
| OLED PWR | P0.26 | P0_26 | `digitalWrite` |
| OLED RST | P0.28 | P0_28 | `digitalWrite` |
| LED 红 | P0.03 | P0_03 | `analogWrite` (TIMER1, 490Hz) |
| LED 绿 | P1.04 | P1_04 | `analogWrite` (TIMER2, 490Hz) |
| LED 蓝 | P1.03 | P1_03 | `analogWrite` (TIMER3, 490Hz) |
| 蜂鸣器 | P0.13 | P0_13 | `nrfx_pwm` (NRF_PWM2, 3kHz, 50%占空比) |
| 振动马达 | P0.14 | P0_14 | `nrfx_pwm` (NRF_PWM3, 20kHz, 78%占空比) |
| 电池 ADC | P0.02 | A2 | `analogRead(A2)` 或 `api.system.bat.get()` |
| 充电检测 | P0.10 | P0_10 | `digitalRead` |
| 红按键 | P0.24 | P0_24 | `digitalRead` + `INPUT_PULLUP` |
| 绿按键 | P0.25 | P0_25 | `digitalRead` + `INPUT_PULLUP` |
| 蓝按键 | P1.01 | P1_01 | `digitalRead` + `INPUT_PULLUP` |
| 黄按键 | P1.02 | P1_02 | `digitalRead` + `INPUT_PULLUP` |
| GPS RX | P0.15 | P0_15 | `Serial1.read()` |
| GPS TX | P0.16 | P0_16 | `Serial1.write()` |

**Hub 引脚映射表：**
| 功能 | NCS 引脚 | RUI3 引脚名 | Arduino API |
|------|---------|-------------|-------------|
| WS2812 LED | P0.15 | P0_15 | Adafruit NeoPixel（`__disable_irq` 保护） |
| LED 电源使能 | P0.24 | P0_24 | `digitalWrite` |
| 蜂鸣器 | P0.09 | P0_09 | `nrfx_pwm` (NRF_PWM0, 3kHz, 50%占空比) |
| 电池 ADC | P0.02 | A2 | `analogRead` |

**LED 驱动方案：**

#### Badge RGB LED（3 路 N-MOS PWM）

3 路 `analogWrite`，TIMER1-3，490Hz：

| LED | 引脚 | API |
|-----|------|-----|
| 红 | P0.03 | `analogWrite(P0_03, r)` |
| 绿 | P1.04 | `analogWrite(P1_04, g)` |
| 蓝 | P1.03 | `analogWrite(P1_03, b)` |

#### Hub WS2812 灯带（P0.15, 15 颗 GL5050RGB01H-T, GRB 序）

P0.15 在 RUI3 定义为 `PIN_SERIAL1_RX`（WisBlock UART1），Hub overlay 中 UART 全部禁用，**空闲可用**。

WS2812 时序：0 码高电平 350ns / 1 码高电平 700ns / 位周期 ~1.2µs / RESET >80µs。三种驱动方式：

| | 方式 1: Adafruit NeoPixel | 方式 2: TIMER+PPI+GPIOTE | 方式 3: SPI (SPIM3) |
|---|---|---|---|
| 原理 | bit-bang (nop 循环) | TIMER CC→PPI→GPIOTE→GPIO | SPI MOSI→P0.15 |
| 抗中断 | 关中断保护 | 纯硬件，免疫 | 纯硬件，免疫 |
| 精度 | 一般 | 15.6ns，100% 正确 | 15.6ns，100% 正确 |
| 复杂度 | 最低 | 需理解 PPI/GPIOTE | 需配置 DMA 缓冲 |
| 状态 | **✅ 采用** | 备选 | 备选 |

**采用方式 1**：Hub 报警显示为单色常亮/闪烁，15 颗灯珠一帧仅 ~432µs 关中断，对 BLE 广播（2s 间隔）无影响。若现场测试出现颜色错误，切换至方式 2 或 3。

```cpp
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel strip(15, P0_15, NEO_GRB + NEO_KHZ800);

void led_strip_update() {
    __disable_irq();     // 关中断 ~432µs
    strip.show();        // 发送 15×24bit
    __enable_irq();
}
```

---

## 项目文件结构

```
arduino_lora_alarm/
├── badge_lora_alarm.ino        # Badge 入口 (#define DEVICE_TYPE 0)
├── hub_lora_alarm.ino          # Hub 入口 (#define DEVICE_TYPE 1)
├── alarm_config.h              # 编译时常量：DEVICE_TYPE, 凭证, 引脚定义
├── debug_macros.h              # LOG_INFO/WARN/ERROR 宏 → Serial.printf
├── app/
│   ├── alarm_sm.h / .cpp       # 告警状态机（纯 C，几乎无改动）
│   ├── actuator_mgr.h / .cpp   # 执行器管理器（纯 C + 驱动调用）
│   ├── power_mgr.h / .cpp      # 电池 ADC → analogRead / api.system.bat
│   ├── join_state.h            # 入网状态枚举（零改动）
│   └── app_hal.h / .cpp        # ✨ 新建：RUI3 LoRaWAN 抽象层
├── proto/
│   ├── proto_internal.h        # 协议常量（移除 #include <zephyr/kernel.h>）
│   ├── proto_parser.h / .cpp   # 帧解析器（k_uptime_get → millis）
│   ├── proto_builder.h / .cpp  # 帧构建器（几乎无改动）
│   ├── proto_handler.h / .cpp  # 命令处理器（替换 LOG 宏）
│   └── proto_crc16.h / .cpp    # CRC16/XMODEM（零改动）
├── config/
│   └── config_store.h / .cpp   # Flash 配置存储（偏移重映射至 0x70000，api.system.flash）
├── ble/
│   ├── ble_badge_scan.h / .cpp # BLE 扫描（Zephyr BLE → api.ble.scanner）
│   └── ble_hub_adv.h / .cpp    # BLE 广播（Zephyr BLE → api.ble.beacon）
├── drv/
│   ├── oled_drv.h / .cpp       # SSD1306（I2C → TWI1 硬件 I2C，字体数据直接复制）
│   ├── led_pwm.h / .cpp        # LED PWM（pwm_set → analogWrite, TIMER1-3, 490Hz）
│   ├── led_strip.h / .cpp      # WS2812（led_strip_update_rgb → NeoPixel+关中断；备选: TIMER+PPI / SPI）
│   ├── buzzer_pwm.h / .cpp     # 蜂鸣器（pwm_set → nrfx_pwm NRF_PWM2/0, 3kHz）
│   └── vibration_gpio.h / .cpp # 振动马达（pwm_set → nrfx_pwm NRF_PWM3, 20kHz）
├── ui/
│   ├── button_sm.h / .cpp      # 按键消抖（gpio_pin_get → digitalRead）
│   └── badge_ui.h / .cpp       # Badge UI 状态机（几乎纯 C）
├── hal/
│   └── hal_gps.h / .cpp        # GPS NMEA 解析（UART ISR → Serial1.poll，解析逻辑直接复制）
├── utils/
│   └── crc32.h / .cpp          # CRC32（零改动）
└── README.md
```

**可以删除的 NCS 模块（RUI3 有内置等效功能）：**
- `hal_sx1262.c` → 被 `app_hal.cpp` 替代
- `hal_sx1262_swl2001.c` → 不需要（RUI3 自带 LoRaWAN 栈）
- `lorawan_classb.c` → 删除（RUI3 内置 Class B）
- `lorawan_mc.c` → 删除（RUI3 内置多播 API）
- `hal_flash.c` → 内联到 `config_store.cpp`
- `serial/at_cmd.c` → 可选（RUI3 自带 AT 命令框架）
- `boards/*.conf`, `boards/*.overlay` → 被 `alarm_config.h` 替代

---

## 实施顺序（7 个阶段）

### 阶段 1：核心基础（第 1-2 天）
创建项目骨架，移植零依赖文件：`crc32`, `proto_crc16`, `proto_internal.h`, `proto_parser`, `proto_builder`, `alarm_sm`。验证：在 Arduino CLI 中编译通过，用 `Serial.println` 测试告警状态机优先级逻辑。

### 阶段 2：LoRaWAN 通信（第 2-3 天）
移植 `config_store`（Flash），编写 `app_hal.cpp`（RUI3 LoRaWAN 包装层），移植 `proto_handler`，实现 `setup()`/`loop()` 初版。验证：在 US915 网关上完成 OTAA 入网，心跳包成功上报到 ChirpStack。

### 阶段 3：硬件外设（第 3-5 天）
移植所有驱动：LED PWM、LED Strip、蜂鸣器、振动马达、电源管理、actuator_mgr。验证：通过下行命令触发告警，LED/蜂鸣器/振动全部响应。

### 阶段 4：Badge UI（第 5-7 天）
移植按键消抖、OLED 驱动（TWI1 硬件 I2C）、badge_ui 状态机、GPS NMEA 解析。验证：按键 3 秒长按→确认提示→再按 2 秒→触发告警→LoRaWAN 上行发送。

### 阶段 5：BLE（第 7-8 天）
移植 Hub BLE 广播和 Badge BLE 扫描。验证：Hub 广播 "ALARM_HUB"，Badge 在扫描周期内检测到，RSSI 和 MAC 上报。

### 阶段 6：高级 LoRaWAN（第 8-9 天）
启用 Class B、4 组多播。验证：Beacon 锁定、多播下行接收。

### 阶段 7：打磨和现场测试（第 9-10 天）
Flash 持久化测试、功耗优化、`millis()` 回绕审计、端到端多设备测试。

---

## 验证策略

- **单元测试**：协议解析器 + 告警状态机 + 按键消抖 — 通过 `Serial.println` 进行台架测试
- **集成测试**：LoRaWAN 入网 → 心跳 → 下行告警 → 执行器链路
- **现场测试**：1 Hub + 3 Badge 同时触发 Code Red，验证延迟、功耗、Class B Beacon 锁定的稳定性

## 关键风险

| 风险 | 缓解措施 |
|------|---------|
| **`nrfx_pwm` 初始化** | 参考 NCS overlay 引脚配置：`NRF_PWM2→P0.13`(蜂鸣器 3kHz), `NRF_PWM3→P0.14`(振动 20kHz)；确认这些引脚未被 RUI3 其他模块占用 |
| **TWI1 OLED 引脚配置** | 确认 RUI3 构建中 TWI1 未被其他模块占用；`I2cMcuInit` 传入 `P0_29`/`P0_30` 即完成引脚映射 |
| **`millis()` 回绕 bug**（每 49 天） | 全面审计所有耗时计算，添加 `WRAP_SAFE_ELAPSED` 宏 |
| **Flash NVM 偏移冲突** | 重映射至 `0x70000`（RUI3 用户 Flash 基址）。LoRaWAN 凭证/DevNonce/会话密钥由 RUI3 `service_nvm` 自动管理，config_store 仅存 4 个业务字段（group_id/device_type/hub_type/room_id） |
