#include <stdint.h>
/*
 * Hub LED 单线级联驱动 — WS2812/GL5050RGB01H-T (GRB 序, 15颗级联)
 *
 * 方式 1: Adafruit NeoPixel + __disable_irq() 保护
 * 15颗灯珠 × 24bit × 1.2µs = ~432µs 关中断, BLE 广播 2s 间隔无影响
 *
 * 参考: ncs_lora_alarm/drv/led_strip.c
 */
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "led_strip.h"
#include "../app/actuator_mgr.h"
#include "../boards/hub/board.h"
extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);


static Adafruit_NeoPixel strip(LED_STRIP_NUM_LEDS, LED_STRIP_PIN,
	LED_STRIP_COLOR_ORDER + LED_STRIP_KHZ);

static struct led_color current_color = {0, 0, 0};
static enum led_mode  current_mode   = LED_MODE_OFF;
static uint16_t       blink_on_ms    = 500;
static uint16_t       blink_off_ms   = 500;
static uint8_t        brightness     = 100;
static bool           blink_on;
static uint32_t       last_toggle_ms;

static uint8_t scale(uint8_t val) {
	return (uint16_t)val * brightness / 100;
}

int led_strip_init(void) {
	pinMode(LED_STRIP_PIN, OUTPUT);
	digitalWrite(LED_STRIP_PIN, LOW);  /* begin 前拉低，避免 WS2812 误解析 */
	delay(1);
	/* 使能 LED 电源 (P0.24 → U2 EN → VDD5) */
	pinMode(LED_PWR_PIN, OUTPUT);
	digitalWrite(LED_PWR_PIN, HIGH);

	strip.begin();
	strip.clear();
	strip.show();
	strip.setBrightness(255);  /* 最大亮度, per-color scaling 由 scale() 处理 */

	SEGGER_RTT_printf(0, "[INFO] LED strip initialized (%d LEDs, pin P0_%d)\r\n",
		LED_STRIP_NUM_LEDS, LED_STRIP_PIN);
	return 0;
}

int led_strip_set_all(struct led_color color) {
	for (int i = 0; i < LED_STRIP_NUM_LEDS; i++) {
		strip.setPixelColor(i,
			scale(color.r),   /* NeoPixel NEO_GRB 自动处理字节序 */
			scale(color.g),
			scale(color.b));
	}
	/* no __disable_irq — SD assert risk */ 
	strip.show();
	
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
	if (index >= LED_STRIP_NUM_LEDS) return -22;
	strip.setPixelColor(index, scale(color.r), scale(color.g), scale(color.b));
	return 0;
}

int led_strip_update(void) {
	/* no __disable_irq — SD assert risk */ 
	strip.show();
	
	return 0;
}

int led_strip_set_brightness(uint8_t pct) {
	if (pct > 100) pct = 100;
	brightness = pct;
	return led_strip_set_all(current_color);
}

int led_strip_off(void) {
	current_mode = LED_MODE_OFF;
	strip.clear();
	/* no __disable_irq — SD assert risk */ 
	strip.show();
	
	return 0;
}

void led_strip_tick(void) {
	if (current_mode == LED_MODE_OFF || current_mode == LED_MODE_ON) return;

	uint32_t now = millis();
	uint32_t elapsed = now - last_toggle_ms;

	if (blink_on && elapsed >= blink_on_ms) {
		blink_on = false;
		last_toggle_ms = now;
		strip.clear();
		/* no __disable_irq — SD assert risk */  strip.show(); 
	} else if (!blink_on && elapsed >= blink_off_ms) {
		blink_on = true;
		last_toggle_ms = now;
		led_strip_set_all(current_color);
	}
}
