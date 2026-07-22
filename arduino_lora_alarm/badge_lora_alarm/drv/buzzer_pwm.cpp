/*
 * 蜂鸣器 PWM 驱动 — nrfx_pwm (NRF_PWM0, 3kHz)
 * N-MOS NPN 8050: PWM HIGH → Q1 导通 → 蜂鸣器响
 * 50% 占空比 = 最大响度 (无源蜂鸣器靠翻转发声)
 *
 * 参考: ncs_lora_alarm/drv/buzzer_pwm.c (NRF_PWM0 ch0, P0.09, 3kHz)
 */
#include <Arduino.h>
#include <nrfx_pwm.h>
#include "buzzer_pwm.h"
#include "../boards/hub/board.h"
#include "../debug_macros.h"

#define TAG "buzzer"

static nrfx_pwm_t pwm_inst = NRFX_PWM_INSTANCE(BUZZER_PWM_INST);
static nrf_pwm_values_individual_t duty_seq[2];
static nrf_pwm_sequence_t const seq = {
	.values.p_individual = duty_seq,
	.length = NRF_PWM_VALUES_LENGTH(duty_seq),
	.repeats = 0,
	.end_delay = 0,
};

/* 状态 */
static enum buzzer_mode mode = BUZZER_OFF;
static uint8_t  volume       = 5;
static uint16_t pattern_on   = 500;
static uint16_t pattern_off  = 500;
static bool     buzz_on;
static uint32_t last_toggle_ms;
static uint32_t start_ms;
static uint32_t auto_stop_sec;

static void output_buzz(bool on) {
	if (on && volume > 0) {
		duty_seq[0] = volume * duty_seq[1] / (2 * BUZZER_VOLUME_MAX);
		nrfx_pwm_simple_playback(&pwm_inst, &seq, 1, NRFX_PWM_FLAG_LOOP);
	} else {
		nrfx_pwm_stop(&pwm_inst, true);
	}
}

int buzzer_pwm_init(void) {
	nrfx_pwm_config_t cfg = {
		.output_pins = { BUZZER_PIN | NRFX_PWM_PIN_INVERTED,
				 NRFX_PWM_PIN_NOT_USED,
				 NRFX_PWM_PIN_NOT_USED,
				 NRFX_PWM_PIN_NOT_USED },
		.irq_priority = APP_IRQ_PRIORITY_LOWEST,
		.base_clock   = NRF_PWM_CLK_16MHz,
		.count_mode   = NRF_PWM_MODE_UP,
		.top_value    = 16000000UL / BUZZER_FREQ_HZ,  /* 16MHz / 5333 ≈ 3kHz */
		.load_mode    = NRF_PWM_LOAD_INDIVIDUAL,
		.step_mode    = NRF_PWM_STEP_AUTO,
	};

	nrfx_err_t err = nrfx_pwm_init(&pwm_inst, &cfg, NULL);
	if (err != NRFX_SUCCESS) {
		LOG_ERROR(TAG, "nrfx_pwm_init failed: %d", err);
		return -5;
	}

	duty_seq[1] = cfg.top_value;
	output_buzz(false);

	LOG_INFO(TAG, "Buzzer initialized (NRF_PWM%d, pin P0_%d, %dHz)",
		BUZZER_PWM_INST, BUZZER_PIN, BUZZER_FREQ_HZ);
	return 0;
}

int buzzer_pwm_set(enum buzzer_mode new_mode, uint8_t vol,
		   uint16_t on_ms, uint16_t off_ms) {
	if (vol > BUZZER_VOLUME_MAX) vol = BUZZER_VOLUME_MAX;

	mode           = new_mode;
	volume         = vol;
	pattern_on     = on_ms;
	pattern_off    = off_ms;
	auto_stop_sec  = (new_mode == BUZZER_ON) ? BUZZER_RED_TIMEOUT_SEC : BUZZER_OTHER_TIMEOUT_SEC;
	start_ms       = millis();
	last_toggle_ms = start_ms;

	switch (mode) {
	case BUZZER_OFF:    buzz_on = false; return output_buzz(false), 0;
	case BUZZER_ON:     buzz_on = true;  return output_buzz(true), 0;
	case BUZZER_PATTERN: buzz_on = true; return output_buzz(true), 0;
	default: return -22;
	}
}

int buzzer_pwm_off(void) {
	mode = BUZZER_OFF;
	buzz_on = false;
	return output_buzz(false), 0;
}

bool buzzer_pwm_is_active(void) { return mode != BUZZER_OFF; }

void buzzer_pwm_tick(void) {
	if (mode == BUZZER_OFF) return;

	uint32_t now = millis();
	uint32_t elapsed_sec = (now - start_ms) / 1000;

	/* 自动停止 */
	if (elapsed_sec >= auto_stop_sec) {
		LOG_INFO(TAG, "Buzzer auto-stop after %ds", auto_stop_sec);
		buzzer_pwm_off();
		return;
	}

	/* 闪烁切换 */
	if (mode == BUZZER_PATTERN) {
		uint32_t elapsed = now - last_toggle_ms;
		if (buzz_on && elapsed >= pattern_on) {
			buzz_on = false; last_toggle_ms = now; output_buzz(false);
		} else if (!buzz_on && elapsed >= pattern_off) {
			buzz_on = true; last_toggle_ms = now; output_buzz(true);
		}
	}
}
