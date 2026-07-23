/*
 * @Author: jiefengzhu focus_feng@163.com
 * @Date: 2026-07-21 23:45:52
 * @LastEditors: jiefengzhu focus_feng@163.com
 * @LastEditTime: 2026-07-22 19:59:41
 * @FilePath: \RUI_lora_alarm\arduino_lora_alarm\hub_lora_alarm\src\boards\hub\board.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
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
#define DEVICE_GROUP_ID      0     /* 默认无角色, 由 CMD 0x50 远程设置 */
#define DEVICE_ROOM_ID       12    /* 默认房间号 */
#define DEVICE_HUB_TYPE      0     /* 0=RoomHub, 1=DoorHub, 2=HallwayHub */

/* ── LoRaWAN OTAA 凭证 (Hub1 — 十二年级教室) ── */
#define OTAA_DEVEUI  {0x20, 0x26, 0x06, 0x18, 0x01, 0x00, 0x00, 0x02}
#define OTAA_APPEUI  {0x8A, 0x61, 0x2A, 0x8B, 0x62, 0x0D, 0x6E, 0xBC}
#define OTAA_APPKEY  {0xB7, 0xD1, 0x5D, 0x51, 0xA8, 0x0A, 0xFF, 0x45, \
                      0x89, 0x67, 0x02, 0x21, 0x9F, 0x35, 0x28, 0x8A}
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

/* ── 蜂鸣器 (NPN 8050 驱动, P0.09, NRF_PWM0 nrfx_pwm) ── */
#define BUZZER_PIN           P0_09
#define BUZZER_PWM_INST      0
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

/* ── LoRaWAN Class B 多播组 (4 组, 静态预配置) ──
 * 参考: ncs_lora_alarm/hal/lorawan_mc.c
 * 共享射频: US915, DR13(SF7/500kHz), 923.3MHz, 信标周期 4s
 */
#define MC_FREQ_HZ           923300000
#define MC_DATARATE          13          /* DR13 = SF7/BW500 */
#define MC_PERIODICITY       2           /* ping 周期 2^2 = 4s */

/* 多播组 0: Code Red */
#define MC_RED_ADDR          0x83C2A6A8
#define MC_RED_NWKSKEY       {0xA1,0xB2,0xC3,0xD4,0xE5,0xF6,0x07,0x18,0x29,0x3A,0x4B,0x5C,0x6D,0x7E,0x8F,0xA0}
#define MC_RED_APPSKEY       {0x1A,0x2B,0x3C,0x4D,0x5E,0x6F,0x70,0x81,0x92,0xA3,0xB4,0xC5,0xD6,0xE7,0xF8,0x09}
/* 多播组 1: Code Blue */
#define MC_BLUE_ADDR         0x1CF26AA9
#define MC_BLUE_NWKSKEY      {0xB2,0xC3,0xD4,0xE5,0xF6,0x07,0x18,0x29,0x3A,0x4B,0x5C,0x6D,0x7E,0x8F,0xA0,0xB1}
#define MC_BLUE_APPSKEY      {0x2B,0x3C,0x4D,0x5E,0x6F,0x70,0x81,0x92,0xA3,0xB4,0xC5,0xD6,0xE7,0xF8,0x09,0x1A}
/* 多播组 2: Code Yellow */
#define MC_YELLOW_ADDR       0xF59367B5
#define MC_YELLOW_NWKSKEY    {0xC3,0xD4,0xE5,0xF6,0x07,0x18,0x29,0x3A,0x4B,0x5C,0x6D,0x7E,0x8F,0xA0,0xB1,0xC2}
#define MC_YELLOW_APPSKEY    {0x3C,0x4D,0x5E,0x6F,0x70,0x81,0x92,0xA3,0xB4,0xC5,0xD6,0xE7,0xF8,0x09,0x1A,0x2B}
/* 多播组 3: Code Green */
#define MC_GREEN_ADDR        0xF7C48FB6
#define MC_GREEN_NWKSKEY     {0xD4,0xE5,0xF6,0x07,0x18,0x29,0x3A,0x4B,0x5C,0x6D,0x7E,0x8F,0xA0,0xB1,0xC2,0xD3}
#define MC_GREEN_APPSKEY     {0x4D,0x5E,0x6F,0x70,0x81,0x92,0xA3,0xB4,0xC5,0xD6,0xE7,0xF8,0x09,0x1A,0x2B,0x3C}

/* ── 系统 ── */
#define HEARTBEAT_INTERVAL_SEC   300    /* 5 分钟心跳 */
#define POWER_REPORT_INTERVAL_SEC 300   /* 5 分钟电量上报 */
#define POWER_REPORT_DELTA_PCT   5      /* 电量变化 ≥5% 才上报 */

#endif /* HUB_BOARD_H */
