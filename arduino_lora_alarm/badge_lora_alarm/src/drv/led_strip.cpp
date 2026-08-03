#include <stdint.h>
/*
 * Badge LED 驱动 — RUI3 analogWrite (NRF_PWM0/1/2, 8-bit, 490Hz)
 *
 * RUI3 analogWrite 内部使用 app_pwm → NRF_PWM 外设:
 *   - Pin → UDRV_PWM_0 → NRF_PWM0 + TIMER1 (LED_R)
 *   - Pin → UDRV_PWM_1 → NRF_PWM1 + TIMER2 (LED_G)
 *   - Pin → UDRV_PWM_2 → NRF_PWM2 + TIMER3 (LED_B)
 * 每个通道独立 PWM 实例, 互不干扰.
 *
 * analogWrite 内部每帧都会 deinit→init (开销 ~100µs), 但对于 LED
 * 闪烁场景 (500ms 量级) 完全不影响.
 *
 * 极性: analogWrite is_invert=1 → APP_PWM_POLARITY_ACTIVE_HIGH
 *       HIGH=LED 亮, 匹配高电平输出硬件.
 *
 * 频率 490Hz, 分辨率 8-bit (0-255).
 */

#include <Arduino.h>
#include "led_strip.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

static struct led_color current_color = {0, 0, 0};
static enum led_mode  current_mode   = LED_MODE_OFF;
static uint16_t       blink_on_ms    = 500;
static uint16_t       blink_off_ms   = 500;
static uint8_t        brightness     = 100;
static bool           blink_on;
static uint32_t       last_toggle_ms;
static bool           hw_ready;

/* 亮度缩放: val 0-255 × brightness 0-100% → 0-255 */
static inline uint8_t led_scale(uint8_t val) {
	if (val == 0 || brightness == 0) return 0;
	uint16_t v = (uint16_t)val * brightness / 100;
	return (v > 255) ? 255 : (uint8_t)v;
}

static void led_apply(struct led_color color) {
	uint8_t r_val = led_scale(color.r);
	uint8_t g_val = led_scale(color.g);
	uint8_t b_val = led_scale(color.b);
	SEGGER_RTT_printf(0, "[LED] analogWrite R=%d(P0_%d) G=%d(P1_%d) B=%d(P1_%d)\n",
		r_val, LED_R_PIN, g_val, LED_G_PIN, b_val, LED_B_PIN);
	analogWrite(LED_R_PIN, r_val);
	analogWrite(LED_G_PIN, g_val);
	analogWrite(LED_B_PIN, b_val);
}

int led_strip_init(void) {
	/* GPIO 初始化为输出低 (PWM 启用前确保 LED 灭) */
	pinMode(LED_R_PIN, OUTPUT);
	pinMode(LED_G_PIN, OUTPUT);
	pinMode(LED_B_PIN, OUTPUT);
	digitalWrite(LED_R_PIN, LOW);
	digitalWrite(LED_G_PIN, LOW);
	digitalWrite(LED_B_PIN, LOW);

	hw_ready = true;

	SEGGER_RTT_printf(0, "[INFO] LED initialized (analogWrite: R=P0_%d G=P1_%d B=P1_%d, 490Hz, 8-bit)\n",
		LED_R_PIN, LED_G_PIN, LED_B_PIN);
	return 0;
}

int led_strip_set_all(struct led_color color) {
	if (!hw_ready) return -1;
	current_color = color;
	led_apply(color);
	return 0;
}

int led_strip_set_mode(enum led_mode mode, struct led_color color,
		       uint16_t on_ms, uint16_t off_ms) {
	if (!hw_ready) return -1;

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
	if (!hw_ready) return -1;
	/* 不修改 current_mode — blink tick 继续正常运转 */
	analogWrite(LED_R_PIN, 0);
	analogWrite(LED_G_PIN, 0);
	analogWrite(LED_B_PIN, 0);
	return 0;
}

void led_strip_tick(void) {
	if (!hw_ready) return;
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
