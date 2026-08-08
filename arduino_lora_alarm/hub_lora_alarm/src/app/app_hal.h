/*
 * LoRaWAN 硬件抽象层 — RUI3 api.lorawan 封装
 * 新建模块, 替代 NCS hal_sx1262.c / lorawan_classb.c / lorawan_mc.c
 */
#ifndef APP_HAL_H
#define APP_HAL_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*lora_downlink_cb_t)(uint8_t port, const uint8_t *data, uint8_t len);

/* ── Beacon 状态机 — Class B beacon 搜索 / 回退 Class A / 自动恢复 ── */
typedef enum {
	BCN_IDLE = 0,
	BCN_SEARCHING,   /* 入网后等待 beacon */
	BCN_LOCKED,      /* Beacon 已同步, Class B 正常 */
	BCN_FALLBACK,    /* 超时回退 Class A */
	BCN_RETRY,       /* 从 Class A 重试获取 beacon */
} beacon_state_t;

void app_hal_lorawan_init(void);
void app_hal_set_downlink_cb(lora_downlink_cb_t cb);
bool app_hal_is_joined(void);
bool app_hal_send(uint8_t fport, const uint8_t *data, uint8_t len, bool confirmed);

/* 入网状态机 (由 loraThread 协程调用) */
void app_hal_join_tick(void);
int  app_hal_get_join_state(void);
bool app_hal_is_beacon_locked(void);
void app_hal_dump_classb_status(void);
void app_hal_lorawan_setup(void);

/* Beacon 状态机 (非阻塞, 由主循环 10s 周期调用) */
void app_hal_beacon_start(void);
void app_hal_beacon_tick(void);
int  app_hal_get_beacon_state(void);

/* 多播组注册 (beacon lock 后调用) */
void app_hal_setup_multicast(void);

#endif
