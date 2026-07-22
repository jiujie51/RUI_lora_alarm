/*
 * 蜂鸣器 PWM 驱动 — nrfx_pwm (NRF_PWM0, 3kHz)
 * Hub: P0.09 → R1(1K) → Q1 NPN Base, PWM HIGH = 导通 = 响
 */
#ifndef BUZZER_PWM_H
#define BUZZER_PWM_H

#include <stdint.h>
#include <stdbool.h>
#include "../app/actuator_mgr.h"  /* enum buzzer_mode, BUZZER_VOLUME_MAX, timeouts */

int  buzzer_pwm_init(void);
int  buzzer_pwm_set(enum buzzer_mode mode, uint8_t volume, uint16_t on_ms, uint16_t off_ms);
int  buzzer_pwm_off(void);
bool buzzer_pwm_is_active(void);
void buzzer_pwm_tick(void);

#endif
