/*
 * Badge Board Configuration
 *
 * 基于 RUI3 WisCore_RAK4631_Board 的引脚定义模式
 * 参考: ncs_lora_alarm/boards/rak4630_badge.overlay + Badge 原理图
 *
 * 硬件: RAK4630 (nRF52840 + SX1262)
 * 外设: 4 按键(R/G/B/Y) + OLED SSD1306 + GPS + 蜂鸣器 + 振动马达 + RGB LED×3
 */

#ifndef BADGE_BOARD_H
#define BADGE_BOARD_H

#include <variant.h>  /* RUI3 RAK4630 基础引脚 */

/* ── 设备身份 ── */
#define DEVICE_TYPE          0     /* 0=Badge */
#define DEVICE_GROUP_ID      0     /* 默认无角色, 由 CMD 0x50 远程设置 */
#define DEVICE_ROOM_ID       0     /* 默认房间号 */

/* ── LoRaWAN OTAA 凭证 (Badge1 — Daniel) ── */
#define OTAA_DEVEUI  {0x20, 0x26, 0x06, 0x18, 0x00, 0x00, 0x00, 0x04}
#define OTAA_APPEUI  {0x5C, 0x16, 0xD8, 0x5A, 0x6E, 0x8C, 0xE7, 0xC9}
#define OTAA_APPKEY  {0xBF, 0x42, 0xEB, 0x63, 0x49, 0x56, 0x4B, 0x67, \
                      0x93, 0xDC, 0xC6, 0x82, 0x7C, 0x52, 0x00, 0x65}
#define OTAA_BAND    RAK_REGION_US915

/* ── 功能开关 ── */
#define GPS_ENABLE           1
#define BLE_OBSERVER         1     /* BLE 扫描 (定位 Hub) */
#define OLED_ENABLE          0

/* ── RGB LED (N-MOS PWM, 低电平导通) ── */
#define LED_R_PIN            P0_03      /* 红灯, analogWrite */
#define LED_G_PIN            P1_04      /* 绿灯, analogWrite */
#define LED_B_PIN            P1_03      /* 蓝灯, analogWrite */
#define LED_PWM_FREQ         490        /* analogWrite 固定频率 Hz */

/* PWM 硬件映射 */
#define LED_R_PWM            UDRV_PWM_0  /* app_pwm TIMER1 */
#define LED_G_PWM            UDRV_PWM_1  /* app_pwm TIMER2 */
#define LED_B_PWM            UDRV_PWM_2  /* app_pwm TIMER3 */

/* ── 蜂鸣器 (NPN 8050 驱动, P0.13) ── */
#define BUZZER_PIN           P0_13
#define BUZZER_PWM_INST      2          /* NRF_PWM2 (nrfx_pwm) */
#define BUZZER_FREQ_HZ       3000

/* ── 振动马达 (N-MOS AO3400 驱动, P0.14) ── */
#define MOTOR_PIN            P0_14
#define MOTOR_PWM_INST       3          /* NRF_PWM3 (nrfx_pwm) */
#define MOTOR_PWM_FREQ       20000

/* ── 按键 (上拉电阻, 按下低电平) ── */
#define BUTTON_RED_PIN       P0_24       /* k1_red */
#define BUTTON_GREEN_PIN     P0_25       /* k2_green */
#define BUTTON_BLUE_PIN      P1_01       /* k3_blue (WB_SW1) */
#define BUTTON_YELLOW_PIN    P1_02       /* k4_yellow (WB_SW2) */
#define BUTTON_DEBOUNCE_MS   30
#define BUTTON_LONG_PRESS_MS 3000

/* ── OLED SSD1306 (I2C 地址 0x3C, 128x64) ── */
#define OLED_SDA_PIN         P0_29       /* TWI1 SDA */
#define OLED_SCL_PIN         P0_30       /* TWI1 SCL */
#define OLED_PWR_PIN         P0_26       /* 供电使能 (高有效) */
#define OLED_RES_PIN         P0_28       /* 复位 (低有效) */
#define OLED_I2C_BUS         1           /* TWI1 */
#define OLED_I2C_ADDR        0x3C
#define OLED_I2C_FREQ        400000      /* 400kHz */

/* ── 电池 (分压 R4=10K + R5=10K, 1/2, ADC AIN0) ── */
#define BATTERY_ADC_PIN      A2          /* P0.02 / AIN0 */
#define BATTERY_FULL_MV      3600
#define BATTERY_EMPTY_MV     3000
#define BATTERY_LOW_PCT      30

/* ── 充电检测 (USB-C 插入检测) ── */
#define CHARGE_DETECT_PIN    P0_10       /* 5V_DC_IN, 高=充电 */
#define CHARGE_ENABLE        1           /* 使能充电检测并在 ALIVE 日志中显示 */

/* ── GPS (RAK12501, u-blox MAX-7Q, 9600bps) ── */
#define GPS_UART             Serial1     /* P0.15 RX / P0.16 TX */
#define GPS_BAUDRATE         9600
#define GPS_PPS_PIN          P0_17       /* 秒脉冲 (可选) */
#define GPS_RESET_PIN        P0_19       /* 复位 (低有效) */

/* ── LoRaWAN Class B 多播组 (与 Hub 共用) ──
 * 密钥来源: doc/design/muticast_flow.md (2026-07-10, ChirpStack 正式密钥)
 * 共享射频: US915, DR3(SF7/125kHz), 923.3MHz, 信标周期 4s
 */
#define MC_FREQ_HZ           923300000
#define MC_DATARATE          3           /* DR3 = SF7/BW125 (匹配 ChirpStack 配置) */
#define MC_PERIODICITY       2           /* ping 周期 2^2 = 4s */

/* 多播组 0: Code Red */
#define MC_RED_ADDR          0x83C2A6A8
#define MC_RED_NWKSKEY       {0xBC,0x6E,0xE9,0x8F,0x74,0x45,0x28,0xA4,0x42,0x2A,0x31,0xF7,0xC3,0xF7,0x16,0x35}
#define MC_RED_APPSKEY       {0x6A,0xA7,0x3D,0x8D,0xAB,0x6D,0x8C,0xBF,0xF1,0xAF,0x3B,0x0A,0xC5,0x31,0xB4,0x93}
/* 多播组 1: Code Blue */
#define MC_BLUE_ADDR         0x1CF26AA9
#define MC_BLUE_NWKSKEY      {0xC2,0xDF,0xF7,0x24,0xD5,0x50,0x05,0x36,0xE2,0x52,0x4B,0xEC,0x84,0xCB,0x41,0xB2}
#define MC_BLUE_APPSKEY      {0xB4,0x96,0x29,0xD7,0x25,0xA9,0xDC,0x4C,0x67,0xD4,0x8C,0xC0,0x49,0x72,0xBB,0xFF}
/* 多播组 2: Code Yellow */
#define MC_YELLOW_ADDR       0xF59367B5
#define MC_YELLOW_NWKSKEY    {0x2C,0xD3,0x6D,0x9D,0x8B,0x83,0xDD,0x29,0x0E,0x8B,0x98,0x65,0x4D,0xB8,0xD4,0xF5}
#define MC_YELLOW_APPSKEY    {0xDD,0xF7,0x8E,0xDF,0xB5,0xDE,0xD0,0x7A,0x8E,0xCE,0xE3,0x66,0x31,0x0E,0x24,0x1B}
/* 多播组 3: Code Green */
#define MC_GREEN_ADDR        0xF7C48FB6
#define MC_GREEN_NWKSKEY     {0xA1,0x51,0xD4,0x61,0xB4,0x40,0x0E,0x11,0x86,0x58,0xB3,0xC1,0xA1,0x42,0xBE,0x5B}
#define MC_GREEN_APPSKEY     {0xA7,0xEF,0x75,0xC7,0x1D,0x98,0xD6,0x4B,0x51,0xA1,0xDC,0xF5,0x1B,0xBC,0x03,0x09}

/* ── 系统 ── */
#define HEARTBEAT_INTERVAL_SEC   300     /* 5 分钟心跳 */
#define POWER_REPORT_INTERVAL_SEC 300    /* 5 分钟电量上报 */
#define POWER_REPORT_DELTA_PCT   5       /* 电量变化 ≥5% 才上报 */

#endif /* BADGE_BOARD_H */
