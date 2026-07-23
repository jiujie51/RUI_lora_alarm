#include <stdint.h>
/*
 * 帧构建器 — 上行帧构造
 * 移植自 NCS: 仅添加 #include <Arduino.h>, ENOSPC 改为 -28
 */
#include <Arduino.h>
#include <string.h>
#include "proto_internal.h"

#ifndef ENOSPC
#define ENOSPC 28
#endif

static int proto_build_frame(uint8_t *buf, uint16_t buf_len,
			     uint8_t cmdid, const uint8_t *data, uint8_t data_len)
{
	uint16_t total_len = PROTO_MIN_FRAME_LEN + data_len;

	if (buf_len < total_len) return -ENOSPC;

	uint8_t *p = buf;
	*p++ = PROTO_HEAD_HI;
	*p++ = PROTO_HEAD_LO;
	*p++ = PROTO_VER;
	*p++ = 0x00; /* control: request packet */
	*p++ = cmdid;
	*p++ = (total_len >> 8) & 0xFF;
	*p++ = total_len & 0xFF;

	/* CRC 占位 */
	uint8_t *crc_ptr = p;
	*p++ = 0;
	*p++ = 0;

	/* data */
	if (data && data_len) {
		memcpy(p, data, data_len);
		p += data_len;
	}

	/* 计算 CRC（跳过 CRC 占位字段 buf[7..8]）*/
	crc16_xmodem_init();
	crc16_xmodem_append(buf, 7);
	crc16_xmodem_append(buf + 9, data_len);
	uint16_t crc = crc16_xmodem_end();

	crc_ptr[0] = (crc >> 8) & 0xFF;
	crc_ptr[1] = crc & 0xFF;

	return total_len;
}

int proto_build_heartbeat(uint8_t *buf, uint16_t buf_len,
			  uint8_t dev_type, uint8_t group_id)
{
	uint8_t data[] = { dev_type, group_id };
	return proto_build_frame(buf, buf_len, CMDID_HEARTBEAT, data, sizeof(data));
}

int proto_build_power(uint8_t *buf, uint16_t buf_len,
		      uint8_t dev_type, uint8_t power_pct)
{
	uint8_t data[] = { dev_type, power_pct };
	return proto_build_frame(buf, buf_len, CMDID_POWER, data, sizeof(data));
}

int proto_build_key_event(uint8_t *buf, uint16_t buf_len,
			  uint8_t button, uint8_t motion, int8_t rssi,
			  const uint8_t hub_mac[6],
			  int32_t lat, int32_t lon)
{
	uint8_t data[18];
	uint8_t *p = data;

	*p++ = internal_button_to_proto(button);
	*p++ = motion;
	*p++ = (uint8_t)(rssi < 0 ? -rssi : rssi);
	memcpy(p, hub_mac, 6); p += 6;
	*p++ = (lat >> 24) & 0xFF;
	*p++ = (lat >> 16) & 0xFF;
	*p++ = (lat >> 8) & 0xFF;
	*p++ = lat & 0xFF;
	*p++ = (lon >> 24) & 0xFF;
	*p++ = (lon >> 16) & 0xFF;
	*p++ = (lon >> 8) & 0xFF;
	*p++ = lon & 0xFF;

	return proto_build_frame(buf, buf_len, CMDID_KEY_EVENT, data, p - data);
}
