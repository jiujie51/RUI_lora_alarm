# LoRa 报警系统 — 开发环境与调试指南

> 版本 V1.0 | 2026-06-19

---

## 1. 技术栈总览

```
┌─────────────────────────────────────────────┐
│ 应用层     │ C (告警状态机/协议引擎/BLE)        │
├─────────────────────────────────────────────┤
│ 中间件     │ LoRaWAN Class B (1.0.3 OTAA)     │
│            │ BLE SoftDevice S140              │
│            │ MCUboot (安全引导 + 双 Bank OTA)  │
├─────────────────────────────────────────────┤
│ RTOS      │ Zephyr RTOS                      │
│            │ 线程/信号量/消息队列/定时器/看门狗  │
├─────────────────────────────────────────────┤
│ 驱动层     │ 见 §2 驱动层详细设计               │
├─────────────────────────────────────────────┤
│ 硬件       │ RAK4630 (nRF52840 + SX1262)      │
│            │ RAK12501 GPS (u-blox MAX-7Q)     │
└─────────────────────────────────────────────┘
```

---

## 2. 驱动层设计（按重要性排序）

### 2.1 GPS 驱动 (RAK12501) ★★★★★

#### 硬件接口

| 参数 | 值 |
|------|-----|
| 模块 | RAK12501 (u-blox MAX-7Q) |
| 通信接口 | **UART** |
| 默认波特率 | **9600 bps** |
| 数据格式 | 8N1 |
| 连接方式 | GPS TX → nRF52840 RX, GPS RX → nRF52840 TX (可选) |
| PPS 引脚 | GPIO (可选，精确授时) |

#### NMEA 解析

| 语句 | 用途 | 关键字段 |
|------|------|---------|
| `$GPGGA` | GPS 定位信息 | UTC 时间、经纬度、定位质量、卫星数、海拔 |
| `$GPRMC` | 推荐最小定位 | 经纬度、速度、日期、磁偏角 |

#### 驱动实现要点

```c
// 驱动架构
hal_gps.c:
  ├── gps_uart_init()         // UART 初始化，9600bps，中断接收
  ├── gps_nmea_isr()          // UART RX ISR → 行缓冲 → 检测 '\n' → 送入解析
  ├── gps_parse_gga()         // 解析 $GPGGA → lat, lon, quality, satellites
  ├── gps_parse_rmc()         // 解析 $GPRMC → lat, lon, speed, date
  ├── gps_get_position()      // 获取最新有效定位 (信号量保护)
  └── gps_is_valid()          // 检查定位是否有效 (quality>0, 30s 超时)
```

#### 异常处理

| 情况 | 处理 |
|------|------|
| 30s 无有效 NMEA | 标记定位失效 |
| UART 帧错误 (噪声) | 丢弃当前行，重新同步 |
| 校验和不匹配 | 丢弃该 NMEA 语句 |
| GPS 无信号 (室内) | lat=90000001, lon=180000001 (无效标记) |
| GPS 恢复 | 首帧有效后立即更新全局坐标 |

#### 调试方式

```bash
# 直连 RAK12501 UART 验证硬件
screen /dev/ttyUSB0 9600
# 应输出: $GPGGA,... / $GPRMC,...
```

---

### 2.2 LED 驱动 ★★★★★

#### 2.2.1 Hub LED — 单线级联驱动

Hub 使用**单线串行级联** LED 驱动协议（类似 WS2812/SK6812），通过一根 GPIO 信号线控制多颗级联灯珠。

**驱动规格**：

| 参数 | 值 |
|------|-----|
| 驱动方式 | **单线串行 (Single-Wire)**，非 PWM |
| 级联方式 | DIN → DO 级联，前级自动整形转发 |
| 信号 | 1 路 GPIO 输出 (D1) |
| MCU 负载 | 仅需驱动第一颗灯珠的 DIN |

**24bit 数据帧结构**：

| 位段 | Bit23~16 | Bit15~8 | Bit7~0 |
|------|----------|---------|--------|
| 颜色 | **G (绿)** | **R (红)** | **B (蓝)** |
| 位序 | MSB First | — | — |

> 纯红色示例：`00000000 11111111 00000000` (G=0, R=255, B=0)

**时序参数**：

| 码型 | T_high | T_low | 周期 |
|------|--------|-------|------|
| **0 码** | 0.20~0.35 μs (typ 0.295) | 0.55~1.2 μs (typ 0.595) | ≥0.89 μs |
| **1 码** | 0.55~1.2 μs (typ 0.595) | 0.20~0.35 μs (typ 0.295) | ≥0.89 μs |
| **RESET** | — | **>80 μs** (建议 >100 μs) | — |

**驱动实现方式（三选一）**：

| 方案 | 原理 | 优缺点 |
|------|------|--------|
| **A. Zephyr WS2812 驱动** ★★★ | `drivers/led_strip/ws2812` | 现成驱动，通过 SPI + DMA 模拟时序，CPU 零干预 |
| **B. nRF PWM + GPIOTE** ★★ | 用 PWM 的 4 个 compare 值模拟 1bit 波形 + DMA | 稳定，但占用 1 路 PWM + 2 个 GPIOTE 通道 |
| **C. 纯 GPIO Bit-Bang** ★ | 关中断后用 NOP 延时精确控制高低电平 | 简单但 CPU 100% 占用，易被中断干扰 |

> **推荐方案 A**：Zephyr 的 `led_strip` 驱动已适配 nRF52840，通过 SPI MOSI 输出时序，DMA 传输不占 CPU。

**Zephyr 设备树配置**：

```dts
/ {
    leds: ws2812_leds {
        compatible = "worldsemi,ws2812-gpio";
        gpios = <&gpio0 16 GPIO_ACTIVE_HIGH>;  /* DIN 引脚，按原理图确定 */
        chain-length = <12>;                     /* 级联灯珠数量 */
        color-mapping = <LED_COLOR_ID_GREEN
                         LED_COLOR_ID_RED
                         LED_COLOR_ID_BLUE>;     /* G→R→B 对应 bit23→bit0 */
    };
};
```

**固件调用示例**：

```c
#include <drivers/led_strip.h>

struct led_rgb colors[12] = {
    {.r = 255, .g = 0, .b = 0},   // 灯珠0: 红色
    {.r = 0, .g = 255, .b = 0},   // 灯珠1: 绿色
    // ...
};
led_strip_update_rgb(dev, colors, 12);
```

**时序容错要求**：

| 约束 | 值 | 说明 |
|------|-----|------|
| 数据帧内低电平中断 | **< 35 μs** | 超过则被误判为 RESET |
| RESET 低电平 | **> 100 μs** | 建议值，保证级联稳定性 |
| 每帧完整发送 | 12 × 24bit = 288bit ≈ 0.26ms (1.25μs/bit) | 帧间 RESET 100μs，总周期 ≈ 0.36ms |

#### 2.2.2 Badge LED — PWM 驱动

Badge 使用 2 颗小型 RGB LED，每颗 3 路 PWM 独立控制。

| 参数 | 值 |
|------|-----|
| 驱动方式 | **PWM** |
| 通道数 | 6 路 (2 颗 LED × RGB 各 3 路) |
| PWM 频率 | **>20 kHz** (避免人耳可闻啸叫) |
| 分辨率 | 8 bit (0~255) |
| 控制方式 | `pwm_set_dt()` 设置占空比 |

**设备树配置**：

```dts
&pwm0 {
    status = "okay";
    ch0-pin = <13>;  /* LED1_R */
    ch1-pin = <14>;  /* LED1_G */
    ch2-pin = <15>;  /* LED1_B */
};

&pwm1 {
    status = "okay";
    ch0-pin = <16>;  /* LED2_R */
    ch1-pin = <17>;  /* LED2_G */
    ch2-pin = <18>;  /* LED2_B */
};
```

---

### 2.3 蜂鸣器驱动 ★★★★

| 参数 | Hub (高分贝) | Badge (小型) |
|------|-------------|-------------|
| 驱动方式 | PWM | PWM |
| 频率 | 2~4 kHz | 2~4 kHz |
| 音量 | 0~10 级 (占空比 + 频率调节) | 0~10 级 |
| MOS 驱动 | 需要 (高功率) | 需要 (小功率) |
| 超时保护 | Red: 60s, 其他: 30s | Red: 60s, 其他: 30s |

---

### 2.4 SX1262 LoRa 驱动 ★★★★★

| 参数 | 值 |
|------|-----|
| 通信接口 | **SPI** |
| SPI 速率 | ≤ 8 MHz |
| 中断引脚 | DIO1 (GPIO 中断 → 唤醒 lora_thread) |
| 复位引脚 | NRESET (GPIO) |
| Busy 引脚 | BUSY (GPIO，可选) |

```c
// 中断 → 信号量 → 线程 的标准模式
void dio1_isr(const struct device *dev, void *arg) {
    k_sem_give(&sem_lora_rx);
}

void lora_thread(void *arg1, void *arg2, void *arg3) {
    while (1) {
        k_sem_take(&sem_lora_rx, K_FOREVER);
        lora_process_irq();  // 处理 TX_DONE / RX_DONE / TX_TIMEOUT / RX_ERROR
    }
}
```

---

### 2.5 按键驱动 (Badge) ★★★★

| 参数 | 值 |
|------|-----|
| 数量 | 4 个 (R/G/B/Y) |
| 接口 | GPIO 中断 (双边沿) |
| 消抖 | 30ms 定时器 |
| 上拉 | 外接上拉电阻 |

```c
// 按键中断 → 信号量 → ui_thread
void button_isr(const struct device *dev, void *arg) {
    k_sem_give(&sem_button);
}
```

---

### 2.6 LCD 驱动 (Badge) ★★★

| 参数 | 值 |
|------|-----|
| 接口 | SPI 或 I2C |
| 规格 | 双行 20 字符 |
| 背光 | GPIO 控制 |

---

### 2.7 振动马达 (Badge) ★★★

| 参数 | 值 |
|------|-----|
| 驱动方式 | GPIO (MOS 管控制) |
| 控制 | 拉高 = 振动, 拉低 = 停止 |

---

### 2.8 电池 ADC ★★★

| 参数 | 值 |
|------|-----|
| 接口 | ADC (nRF52840 SAADC) |
| 采样 | 分压电阻 1:1 或 1:2 |
| 精度 | ±5% |
| 采样间隔 | 每 30s |

---

### 2.9 Flash 配置存储 ★★★

| 参数 | 值 |
|------|-----|
| 存储介质 | nRF52840 内部 Flash 最后 4KB 页 |
| 数据 | `device_config_t` (group_id, alarm_cfgs[8], ping_slot, ...) |
| 完整性 | CRC32 校验 |
| 写入策略 | 写新页 → 擦旧页 → 磨损均衡 |

---

### 2.10 看门狗 ★★

| 参数 | 值 |
|------|-----|
| 来源 | nRF52840 内部 WDT |
| 超时 | 120s |
| 喂狗 | sys_workq 每 30s |

---

## 3. 开发环境准备

### 3.1 推荐方案

| 方案 | 适用度 | 说明 |
|------|--------|------|
| **Linux (Ubuntu 22.04)** | ★★★★★ | 工具链最稳定 |
| **WSL2 (Ubuntu 22.04)** | ★★★★ | Windows 下首选 |
| **Windows + Toolchain Manager** | ★★★ | nRF 官方一键安装 |

### 3.2 安装步骤

```bash
# ── Step 1: 系统依赖 ──
sudo apt update && sudo apt install --no-install-recommends \
    git cmake ninja-build gperf ccache dfu-util \
    device-tree-compiler wget python3-dev python3-pip \
    python3-setuptools python3-venv python3-wheel \
    xz-utils file make gcc gcc-multilib g++-multilib \
    libsdl2-dev libmagic1

# ── Step 2: West ──
pip3 install --user west
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

# ── Step 3: nRF Connect SDK v2.6.1 ──
mkdir ~/ncs && cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v2.6.1
west update
west zephyr-export
pip3 install -r zephyr/scripts/requirements.txt
pip3 install -r nrf/scripts/requirements.txt
pip3 install -r bootloader/mcuboot/scripts/requirements.txt

# ── Step 4: ARM GCC ──
wget https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2
tar xf gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=~/gcc-arm-none-eabi-10.3-2021.10

# ── Step 5: nRF 命令行工具 ──
# 下载: https://www.nordicsemi.com/Products/Development-tools/nRF-Command-Line-Tools/Download
sudo dpkg -i nrf-command-line-tools_*.deb

# ── Step 6: 验证 ──
west --version
arm-none-eabi-gcc --version
nrfjprog --version
```

---

## 4. 编译与烧录

```bash
# 编译
west build -b rak4630_hub   -d build/hub       # Hub
west build -b rak4630_badge -d build/badge     # Badge
west build -b native_posix  -d build/test -- -DCONFIG_TEST=y  # 单元测试

# 烧录
west flash -d build/hub
nrfjprog --program build/hub/zephyr/merged.hex --chiperase --reset  # 全擦

# 单元测试运行
./build/test/zephyr/zephyr.exe
```

---

## 5. 调试方法

### 5.1 串口日志 (★★★ 最常用)

```bash
# Linux
screen /dev/ttyACM0 115200     # 日志串口 (RAK4630)
screen /dev/ttyUSB0 9600        # GPS 直读 (RAK12501)
```

```c
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(hub, LOG_LEVEL_DBG);
LOG_INF("LED strip update: %d pixels", count);
LOG_DBG("GPS NMEA: %s", nmea_buf);
LOG_ERR("CRC16 fail: calc=0x%04X recv=0x%04X", calc, recv);
```

### 5.2 J-Link RTT (★★★ 无需 UART 线)

```bash
JLinkRTTViewer       # GUI 工具
JLinkRTTClient       # 命令行
```

### 5.3 GDB 硬件调试

```bash
# 终端1
JLinkGDBServer -device nRF52840_xxAA -if SWD -speed 4000
# 终端2
arm-none-eabi-gdb build/hub/zephyr/zephyr.elf
(gdb) target remote localhost:2331
(gdb) b alarm_sm.c:alarm_set
(gdb) c
```

### 5.4 LED 单线协议调试

```bash
# 逻辑分析仪抓取 D1 信号线
# 参数: 采样率 ≥ 20 MHz, 触发: 上升沿, 解码: 自定义 1-Wire 或 WS2812 协议分析器

# 固件端排查
LOG_HEXDUMP_DBG(led_buffer, sizeof(led_buffer), "LED Frame");
```

### 5.5 BLE 调试

| 工具 | 用途 |
|------|------|
| **nRF Connect (手机 App)** | 扫描 Hub 广播，查看 Manufacturer Data |
| **nRF Connect (桌面版)** + nRF52840 Dongle | BLE Sniffer |

### 5.6 LoRa 调试

| 工具 | 用途 |
|------|------|
| **ChirpStack V4 Web UI** | 查看 Device Frames (原始 HEX、RSSI/SNR) |
| **MQTTX / MQTT Explorer** | 监控 ChirpStack MQTT 推送 |

---

## 6. prj.conf 关键配置

```ini
# ── 内核 ──
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_HEAP_MEM_POOL_SIZE=16384
CONFIG_NUM_PREEMPT_PRIORITIES=8

# ── 日志 ──
CONFIG_LOG=y
CONFIG_LOG_MODE_DEFERRED=y

# ── 外设 ──
CONFIG_GPIO=y
CONFIG_PWM=y              # Badge LED + 蜂鸣器
CONFIG_ADC=y              # 电池
CONFIG_UART=y             # ★ GPS UART
CONFIG_SPI=y              # SX1262 + (Badge LCD)

# ── LED Strip (Hub 单线级联) ──
CONFIG_LED_STRIP=y        # ★ Zephyr WS2812 驱动

# ── LoRaWAN ──
CONFIG_LORAWAN=y
CONFIG_LORAWAN_CLASS_B=y
CONFIG_LORAWAN_ACTIVATION_OTAA=y

# ── BLE ──
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y    # Hub Broadcaster
CONFIG_BT_OBSERVER=y      # Badge Scanner

# ── Flash ──
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_BOOTLOADER_MCUBOOT=y

# ── Watchdog ──
CONFIG_WATCHDOG=y
```

---

## 7. 硬件调试接口

```
PC ──USB──→ J-Link ──SWD──→ RAK4630           (烧录/调试)
PC ──USB──→ USB-UART ──→ RAK4630 UART           (日志 115200)
PC ──USB──→ USB-UART ──→ RAK12501 UART          (GPS 直读 9600)
PC ──USB──→ 逻辑分析仪 ──→ Hub LED D1 信号线      (LED 协议分析)
```

---

## 8. 快速验证检查清单

- [ ] nRF Connect SDK v2.6.1 安装完成
- [ ] `west build -b rak4630_hub` 编译通过
- [ ] `west flash` 烧录成功，串口有日志输出
- [ ] GPS UART 直读收到 NMEA 语句
- [ ] **Hub LED 单线协议**：逻辑分析仪确认 D1 波形符合时序参数
- [ ] BLE 广播：手机 App 扫描到 "ALARM_HUB"
- [ ] ChirpStack：网关在线，设备 OTAA 入网
- [ ] 心跳帧在 ChirpStack Device Frames 可见

---

## 9. 命令速查

```bash
# 编译
west build -b rak4630_hub -d build/hub
west build -b rak4630_badge -d build/badge
west build -b native_posix -d build/test -- -DCONFIG_TEST=y

# 烧录
west flash -d build/hub
nrfjprog --program merged.hex --chiperase --reset

# 串口
screen /dev/ttyACM0 115200     # 日志
screen /dev/ttyUSB0 9600       # GPS 直读

# RTT
JLinkRTTViewer

# GDB
JLinkGDBServer -device nRF52840_xxAA -if SWD
arm-none-eabi-gdb build/hub/zephyr/zephyr.elf

# OTA
mcumgr -c serial image upload build/hub/zephyr/app_update.bin

# 测试
./build/test/zephyr/zephyr.exe
```
