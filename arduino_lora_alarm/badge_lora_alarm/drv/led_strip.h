/*
 * Hub LED 单线级联驱动 — WS2812/GL5050RGB01H-T + Adafruit NeoPixel
 *
 * 硬件: P0.15 → LED1 DIN → DOUT → LED2 DIN → ... → LED15 DOUT (15颗 GRB 序)
 * 供电: P0.24 → U2 EN → VDD5
 * 方式 1: Adafruit NeoPixel + __disable_irq() 保护 (~432µs 关中断)
 * 备选: 方式 2 (TIMER+PPI+GPIOTE) / 方式 3 (SPI)
 */
#ifndef LED_STRIP_H
#define LED_STRIP_H

#include <stdint.h>

#define LED_STRIP_NUM_LEDS  15

struct led_color { uint8_t r, g, b; };
enum led_mode { LED_MODE_OFF = 0, LED_MODE_ON, LED_MODE_BLINK, LED_MODE_BREATH, LED_MODE_FAST = 4 };

int  led_strip_init(void);
int  led_strip_set_all(struct led_color color);
int  led_strip_set_mode(enum led_mode mode, struct led_color color, uint16_t on_ms, uint16_t off_ms);
int  led_strip_set_led(uint8_t index, struct led_color color);
int  led_strip_update(void);
int  led_strip_set_brightness(uint8_t pct);
int  led_strip_off(void);
void led_strip_tick(void);

#endif
