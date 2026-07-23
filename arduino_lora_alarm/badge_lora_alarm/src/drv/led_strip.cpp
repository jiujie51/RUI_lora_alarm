#include <stdint.h>
/*
 * Badge LED 驱动 — udrv_pwm 直驱, 3 端口永久绑定
 *
 * 硬件: N-MOS PWM, udrv_pwm is_invert=1 → set_duty(0)=全亮, set_duty(255)=全灭
 *       LED_R: UDRV_PWM_0 (P0_03), LED_G: UDRV_PWM_1 (P1_04), LED_B: UDRV_PWM_2 (P1_03)
 *       频率: 490Hz
 *
 * 不使用 analogWrite(): 避免每次 deinit/reinit 导致端口池冲突.
 * 蜂鸣器 (TIMER4) 和 LED (UDRV_PWM_0/1/2) 完全独立.
 *
 * 参考: hub_lora_alarm/src/drv/led_strip.cpp (WS2812 版本)
 *       badge board.h 引脚定义
 *       RUI3 udrv_pwm.h API
 */
#include <Arduino.h>
#include "led_strip.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

extern "C" {
#include "udrv_pwm.h"
}

/* ── 端口映射 ── */
#define LED_R_PORT  UDRV_PWM_0
#define LED_G_PORT  UDRV_PWM_1
#define LED_B_PORT  UDRV_PWM_2
#define LED_FREQ_HZ 490

static struct led_color current_color = {0, 0, 0};
static enum led_mode  current_mode   = LED_MODE_OFF;
static uint16_t       blink_on_ms    = 500;
static uint16_t       blink_off_ms   = 500;
static uint8_t        brightness     = 100;
static bool           blink_on;
static uint32_t       last_toggle_ms;

/* N-MOS is_invert=1: udrv_pwm_set_duty(0)=全亮, set_duty(255)=全灭 */
static inline uint32_t led_duty(uint8_t val) {
	return 255 - (uint16_t)val * brightness / 100;
}

static void led_port_set(udrv_pwm_port port, uint32_t duty) {
	udrv_pwm_set_duty(port, duty);
}

int led_strip_init(void) {
	/* 3 路 LED 各占一个 UDRV_PWM 端口, init 一次不再变动 */
	udrv_pwm_init(LED_R_PORT, LED_FREQ_HZ, 1, LED_R_PIN);
	udrv_pwm_enable(LED_R_PORT, PWM_NO_TIMEOUT);

	udrv_pwm_init(LED_G_PORT, LED_FREQ_HZ, 1, LED_G_PIN);
	udrv_pwm_enable(LED_G_PORT, PWM_NO_TIMEOUT);

	udrv_pwm_init(LED_B_PORT, LED_FREQ_HZ, 1, LED_B_PIN);
	udrv_pwm_enable(LED_B_PORT, PWM_NO_TIMEOUT);

	led_strip_off();

	SEGGER_RTT_printf(0, "[INFO] LED initialized (udrv_pwm: R=P0_%d G=P1_%d B=P1_%d, %dHz)\n",
		LED_R_PIN, LED_G_PIN, LED_B_PIN, LED_FREQ_HZ);
	return 0;
}

int led_strip_set_all(struct led_color color) {
	led_port_set(LED_R_PORT, led_duty(color.r));
	led_port_set(LED_G_PORT, led_duty(color.g));
	led_port_set(LED_B_PORT, led_duty(color.b));
	return 0;
}

int led_strip_set_mode(enum led_mode mode, struct led_color color,
		       uint16_t on_ms, uint16_t off_ms) {
	current_mode  = mode;
	current_color = color;
	blink_on_ms   = on_ms;
	blink_off_ms  = off_ms;
	blink_on      = true;
	last_toggle_ms = millis();

	switch (mode) {
	case LED_MODE_OFF:  return led_strip_off();
	case LED_MODE_ON:   return led_strip_set_all(color);
	default:            return led_strip_set_all(color);
	}
}

int led_strip_set_led(uint8_t index, struct led_color color) {
	(void)index; (void)color;
	return 0;
}

int led_strip_update(void) {
	return led_strip_set_all(current_color);
}

int led_strip_set_brightness(uint8_t pct) {
	if (pct > 100) pct = 100;
	brightness = pct;
	return led_strip_set_all(current_color);
}

int led_strip_off(void) {
	current_mode = LED_MODE_OFF;
	led_port_set(LED_R_PORT, 255);
	led_port_set(LED_G_PORT, 255);
	led_port_set(LED_B_PORT, 255);
	return 0;
}

void led_strip_tick(void) {
	if (current_mode == LED_MODE_OFF || current_mode == LED_MODE_ON) return;

	uint32_t now = millis();
	uint32_t elapsed = now - last_toggle_ms;

	if (blink_on && elapsed >= blink_on_ms) {
		blink_on = false;
		last_toggle_ms = now;
		led_strip_off();
	} else if (!blink_on && elapsed >= blink_off_ms) {
		blink_on = true;
		last_toggle_ms = now;
		led_strip_set_all(current_color);
	}
}
