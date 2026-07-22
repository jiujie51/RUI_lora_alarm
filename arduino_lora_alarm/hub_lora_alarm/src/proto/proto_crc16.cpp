#include <stdint.h>
/*
 * CRC16/XMODEM — 应用层帧校验
 * 直接复制自 NCS, 零依赖
 *
 * 验证向量（来自协议文档 V1.4）:
 *   crc_data = {0xAA,0x55,0x01,0x00,0x00,0x08,0x00,0x03}
 *   crc16_xmodem 结果: 0x58C7
 */
#include "proto_internal.h"

static uint16_t xmodem_val;

void crc16_xmodem_init(void)
{
	xmodem_val = 0;
}

void crc16_xmodem_append(const uint8_t *buf, uint16_t len)
{
	while (len--) {
		xmodem_val ^= (uint16_t)(*buf++) << 8;
		for (uint8_t i = 0; i < 8; i++) {
			if (xmodem_val & 0x8000) {
				xmodem_val = (xmodem_val << 1) ^ 0x1021;
			} else {
				xmodem_val <<= 1;
			}
		}
	}
}

uint16_t crc16_xmodem_end(void)
{
	return xmodem_val;
}
