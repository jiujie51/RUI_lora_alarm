/*
 * Hub Board Configuration
 *
 * 基于 RUI3 WisCore_RAK4631_Board 的引脚定义模式
 * 参考: ncs_lora_alarm/boards/rak4630_hub.overlay + Hub 原理图
 *
 * 硬件: RAK4630 (nRF52840 + SX1262)
 * 外设: WS2812 LED 灯带 (15颗) + 蜂鸣器
 *
 * 注: Hub 无 LCD、无按键（仅复位键）、无振动马达、无 GPS
 */

#ifndef HUB_BOARD_H
#define HUB_BOARD_H

#include <variant.h>  /* RUI3 RAK4630 基础引脚 */

/* ── 设备身份 ── */
#define DEVICE_TYPE          1     /* 1=Hub */
#define DEVICE_ROOM_ID       12    /* 默认房间号 (十二年级教室) */
#define DEVICE_HUB_TYPE      0     /* 0=RoomHub, 1=DoorHub, 2=HallwayHub */

/* ── LoRaWAN OTAA 凭证 (Hub1 — 十二年级教室) ── */
#define OTAA_DEVEUI  {0x20, 0x26, 0x06, 0x18, 0x01, 0x00, 0x00, 0x01}
#define OTAA_APPEUI  {0xD3, 0xA1, 0x80, 0x5B, 0x54, 0xFA, 0x11, 0x85}
#define OTAA_APPKEY  {0x50, 0xAD, 0x90, 0x7A, 0xF0, 0x5F, 0x8C, 0x5B, \
                      0x45, 0x0F, 0x82, 0x1D, 0x83, 0x21, 0x67, 0x88}
#define OTAA_BAND    RAK_REGION_US915

/* ── 功能开关 ── */
#define GPS_ENABLE           0     /* Hub 无 GPS */
#define BLE_OBSERVER         0     /* Hub 是 Broadcaster, 不是 Scanner */
#define OLED_ENABLE          0

/* ── WS2812 LED 灯带 (GL5050RGB01H-T, 15颗级联, GRB 序) ── */
#define LED_STRIP_PIN        P0_15      /* LED1 DIN 数据线 */
#define LED_STRIP_NUM_LEDS   15
#define LED_STRIP_COLOR_ORDER NEO_GRB
#define LED_STRIP_KHZ        NEO_KHZ800
#define LED_PWR_PIN          P0_24      /* U2 EN → VDD5 LED 供电使能 (高有效) */

/* ── 蜂鸣器 (NPN 8050 驱动, P0.09) ── */
#define BUZZER_PIN           P0_09
#define BUZZER_PWM_INST      0          /* NRF_PWM0 (nrfx_pwm) */
#define BUZZER_FREQ_HZ       3000

/* ── 电池 (分压 R5=10K→VBAT, R4=10K→GND, 1/2, ADC AIN0) ── */
#define BATTERY_ADC_PIN      A2          /* P0.02 / AIN0 */
#define BATTERY_FULL_MV      3600
#define BATTERY_EMPTY_MV     3000
#define BATTERY_LOW_PCT      30

/* ── Hub 无充电检测 ── */
#define CHARGE_ENABLE        0

/* ── BLE 广播配置 ── */
#define BLE_ADV_NAME         "ALARM_HUB"
#define BLE_ADV_INTERVAL_MS  2000       /* 2s 广播间隔 */

/* ── 系统 ── */
#define HEARTBEAT_INTERVAL_SEC   300    /* 5 分钟心跳 */
#define POWER_REPORT_INTERVAL_SEC 300   /* 5 分钟电量上报 */
#define POWER_REPORT_DELTA_PCT   5      /* 电量变化 ≥5% 才上报 */

#endif /* HUB_BOARD_H */
