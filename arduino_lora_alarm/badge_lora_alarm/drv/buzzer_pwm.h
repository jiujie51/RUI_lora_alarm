/*
 * 蜂鸣器 PWM 驱动 — nrfx_pwm (NRF_PWM0, 3kHz)
 * Hub: P0.09 → R1(1K) → Q1 NPN Base, PWM HIGH = 导通 = 响
 */
#ifndef BUZZER_PWM_H
#define BUZZER_PWM_H

#include <stdint.h>
#include <stdbool.h>

enum buzzer_mode { BUZZER_OFF = 0, BUZZER_ON, BUZZER_PATTERN };

#define BUZZER_VOLUME_MAX      10
#define BUZZER_RED_TIMEOUT_SEC 60
#define BUZZER_OTHER_TIMEOUT_SEC 30

int  buzzer_pwm_init(void);
int  buzzer_pwm_set(enum buzzer_mode mode, uint8_t volume, uint16_t on_ms, uint16_t off_ms);
int  buzzer_pwm_off(void);
bool buzzer_pwm_is_active(void);
void buzzer_pwm_tick(void);

#endif
