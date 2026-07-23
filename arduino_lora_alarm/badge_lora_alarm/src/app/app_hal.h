/*
 * LoRaWAN 硬件抽象层 — RUI3 api.lorawan 封装
 */
#ifndef APP_HAL_H
#define APP_HAL_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*lora_downlink_cb_t)(uint8_t port, const uint8_t *data, uint8_t len);

void app_hal_lorawan_init(void);
void app_hal_set_downlink_cb(lora_downlink_cb_t cb);
bool app_hal_is_joined(void);
bool app_hal_send(uint8_t fport, const uint8_t *data, uint8_t len, bool confirmed);

/* 入网状态机 (由 loraThread 协程调用) */
void app_hal_join_tick(void);
int  app_hal_get_join_state(void);
bool app_hal_is_beacon_locked(void);

/* 多播组注册 (beacon lock 后调用) */
void app_hal_setup_multicast(void);

#endif
