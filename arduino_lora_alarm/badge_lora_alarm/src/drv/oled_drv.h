/*
 * OLED SSD1306 驱动 — RUI3 hal_i2c (NRF_TWIM0, 硬件 I2C)
 *
 * I2C: TWI0 @ P0.29(SDA) / P0.30(SCL), 400kHz
 * API: rak_i2c_init / rak_i2c_simple_write
 *
 * 显示函数: 直接写 GDDRAM, 无帧缓冲
 */
#ifndef OLED_DRV_H
#define OLED_DRV_H

#include <stdint.h>
#include <stdbool.h>

#define OLED_W  128
#define OLED_H  64

int  oled_init(void);
void oled_clear(void);                       /* 清屏 (全黑), 对齐 OLED_Clear */
void oled_fill_screen(uint8_t pattern);      /* 全屏填充, 对齐 fill_picture */
void oled_draw_string(int x, int page, const char *str);  /* 字符串, page=页号0-7 */
void oled_display_on(void);                  /* 开显示 (0xAF) */
void oled_display_off(void);                 /* 关显示 (0xAE) */
void oled_clear_line(uint8_t page);          /* 清空 2 行 (page ~ page+1, 16px) */

#endif
