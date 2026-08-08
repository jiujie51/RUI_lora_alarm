#include <stdint.h>
/*
 * 蜂鸣器驱动 — RUI3 tone()/noTone() 版本
 *
 * NPN 8050 驱动: tone() 产生 3kHz 方波 → Q1 导通 → 蜂鸣器响
 * 音量控制: tone() 固定 50% 占空比, volume 仅做 on/off 开关
 *
 * 注: 本文件使用 tone()/noTone() 而非 nrfx_timer TIMER4,
 *     避免与 NeoPixel (PWM0+DMA) 产生硬件资源冲突.
 *
 * 参考: ncs_lora_alarm/drv/buzzer_pwm.c (NRF_PWM0 ch0, P0.09, 3kHz)
 */
#include <Arduino.h>
#include "buzzer_pwm.h"
#include "../boards/hub/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 蜂鸣器自检 ──
 * 1 = init 后蜂鸣 3 声短促滴, 确认硬件正常
 * 0 = 跳过
 */
#define BUZZER_SELF_TEST  0


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
		tone(BUZZER_PIN, BUZZER_FREQ_HZ);
	} else {
		noTone(BUZZER_PIN);
	}
}

int buzzer_pwm_init(void) {
	pinMode(BUZZER_PIN, OUTPUT);
	noTone(BUZZER_PIN);

	SEGGER_RTT_printf(0, "[INFO] Buzzer initialized (tone API, pin P0_%d, %dHz)\r\n",
		BUZZER_PIN, BUZZER_FREQ_HZ);

#if BUZZER_SELF_TEST
	/* 自检: 3 声短促滴 (100ms on / 200ms off) */
	SEGGER_RTT_printf(0, "[INFO] Buzzer self-test: 3 beeps\r\n");
	for (int i = 0; i < 3; i++) {
		tone(BUZZER_PIN, BUZZER_FREQ_HZ);
		delay(100);
		noTone(BUZZER_PIN);
		if (i < 2) delay(200);
	}
#endif

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
	case BUZZER_OFF:
		buzz_on = false;
		output_buzz(false);
		return 0;
	case BUZZER_ON:
		buzz_on = true;
		output_buzz(true);
		return 0;
	case BUZZER_PATTERN:
		buzz_on = true;
		output_buzz(true);
		return 0;
	default:
		return -22;
	}
}

int buzzer_pwm_off(void) {
	mode = BUZZER_OFF;
	buzz_on = false;
	output_buzz(false);
	return 0;
}

bool buzzer_pwm_is_active(void) { return mode != BUZZER_OFF; }

void buzzer_pwm_tick(void) {
	if (mode == BUZZER_OFF) return;

	uint32_t now = millis();
	uint32_t elapsed_sec = (now - start_ms) / 1000;

	/* 自动停止 */
	if (elapsed_sec >= auto_stop_sec) {
		SEGGER_RTT_printf(0, "[INFO] Buzzer auto-stop after %ds\r\n", auto_stop_sec);
		buzzer_pwm_off();
		return;
	}

	/* 闪烁切换 */
	if (mode == BUZZER_PATTERN) {
		uint32_t elapsed = now - last_toggle_ms;
		if (buzz_on && elapsed >= pattern_on) {
			buzz_on = false;
			last_toggle_ms = now;
			output_buzz(false);
		} else if (!buzz_on && elapsed >= pattern_off) {
			buzz_on = true;
			last_toggle_ms = now;
			output_buzz(true);
		}
	}
}
