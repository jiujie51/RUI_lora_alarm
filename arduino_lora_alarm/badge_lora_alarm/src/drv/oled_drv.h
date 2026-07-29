/*
 * OLED SSD1306 驱动 — 直接寄存器操作 NRF_TWIM1 (绕过 RUI3 Wire1 缺陷)
 *
 * RUI3 core.a 预编译时 TWI1_ENABLED=0, Wire1 硬件层被裁剪.
 * 本驱动直接操作 NRF_TWIM1 寄存器: P0.29(SDA) / P0.30(SCL).
 *
 * 显示函数对齐 examples/OLED/oled.c: 直接写 GDDRAM, 无帧缓冲.
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
