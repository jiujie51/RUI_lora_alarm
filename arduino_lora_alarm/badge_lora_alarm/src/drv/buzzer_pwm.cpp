#include <stdint.h>
/*
 * 蜂鸣器驱动 — nrfx_timer TIMER4 + 高优先级 ISR
 *
 * TIMER4 @ 16MHz, CC0 = 2667 → 3kHz 方波, ISR 翻转 GPIO.
 * 中断优先级设为 3 (高于 SoftDevice Radio ISR=0, 但低于 Scheduler=2).
 *
 * nRF52840 中断优先级: 0=最高, 7=最低.
 * SoftDevice S140: Radio=0(专有), Scheduler=1, 应用可用 2-7.
 * TIMER4 用优先级 2 可保证不被 BLE 长时间阻塞.
 *
 * 注意: NFC 也声明了 TIMER4(nrfx_nfct.c), 必须先关闭 NFC.
 */
#include <Arduino.h>
#include <nrfx_timer.h>
#include <hal/nrf_gpio.h>
#include "buzzer_pwm.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

#define BUZZER_TIMER_INST  4
#define BUZZER_TICKS       2667  /* 16MHz / (2 * 3000Hz) */
#define BUZZER_IRQ_PRIO    3     /* 高于 SoftDevice Radio(0) 以外的所有 BLE 操作 */

static const nrfx_timer_t buzzer_timer = NRFX_TIMER_INSTANCE(BUZZER_TIMER_INST);
static volatile bool hw_ready = false;

static enum buzzer_mode mode = BUZZER_OFF;
static uint8_t  volume       = 5;
static uint16_t pattern_on   = 500;
static uint16_t pattern_off  = 500;
static bool     buzz_on;
static uint32_t last_toggle_ms;
static uint32_t start_ms;
static uint32_t auto_stop_sec;

/* ── TIMER4 ISR: 翻转蜂鸣器引脚 ── */
static void buzzer_timer_handler(nrf_timer_event_t event_type, void *ctx) {
	(void)ctx;
	if (event_type == NRF_TIMER_EVENT_COMPARE0) {
		nrf_gpio_pin_toggle(BUZZER_PIN);
	}
}

static void output_buzz(bool on) {
	if (!hw_ready) return;
	if (on && volume > 0) {
		nrfx_timer_clear(&buzzer_timer);
		nrfx_timer_enable(&buzzer_timer);
	} else {
		nrfx_timer_disable(&buzzer_timer);
		nrf_gpio_pin_write(BUZZER_PIN, 0);
	}
}

int buzzer_pwm_init(void) {
	nrf_gpio_cfg_output(BUZZER_PIN);
	nrf_gpio_pin_write(BUZZER_PIN, 0);

	nrfx_timer_config_t cfg = NRFX_TIMER_DEFAULT_CONFIG;
	cfg.frequency = NRF_TIMER_FREQ_16MHz;
	cfg.interrupt_priority = BUZZER_IRQ_PRIO;  /* 优先级 3, BLE 阻塞不了 */

	nrfx_err_t err = nrfx_timer_init(&buzzer_timer, &cfg, buzzer_timer_handler);
	if (err != NRFX_SUCCESS) {
		SEGGER_RTT_printf(0, "[ERROR] nrfx_timer_init(TIMER4) failed: %d\n", err);
		return -5;
	}

	SEGGER_RTT_printf(0, "[BUZZ] TIMER4 IRQ prio=%d (was default=%d)\n",
		BUZZER_IRQ_PRIO, NRFX_TIMER_DEFAULT_CONFIG_IRQ_PRIORITY);

	/* COMPARE0: 2667 ticks 翻转, 自动清零, 使能中断 */
	nrfx_timer_extended_compare(&buzzer_timer, NRF_TIMER_CC_CHANNEL0,
		BUZZER_TICKS,
		NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK,
		true);

	hw_ready = true;

	SEGGER_RTT_printf(0, "[INFO] Buzzer initialized (TIMER4 ISR, pin P0_%d, %dHz, prio=%d)\n",
		BUZZER_PIN, BUZZER_FREQ_HZ, BUZZER_IRQ_PRIO);

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

	if (elapsed_sec >= auto_stop_sec) {
		SEGGER_RTT_printf(0, "[INFO] Buzzer auto-stop after %ds\n", auto_stop_sec);
		buzzer_pwm_off();
		return;
	}

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
