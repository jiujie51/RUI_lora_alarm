#include <stdint.h>
/*
 * 帧解析器 — 整 buffer 直接解析 (RUI3 recv_cb 模型)
 * 串口 AT 由 RAK 内部处理, 此模块仅服务 LoRaWAN 下行完整帧
 */
#include <Arduino.h>
#include <string.h>
#include "proto_internal.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/*
 * 解析完整帧 buffer, 校验通过后调用 proto_handle_frame
 * 返回: 0=成功, -1=长度不足, -2=帧头错误, -3=CRC错误
 */
int proto_parser_parse(const uint8_t *data, uint8_t len)
{
	if (len < PROTO_MIN_FRAME_LEN) {
		SEGGER_RTT_printf(0, "[PARSER] too short: %u < %u\n", len, PROTO_MIN_FRAME_LEN);
		return -1;
	}

	if (data[0] != PROTO_HEAD_HI || data[1] != PROTO_HEAD_LO) {
		SEGGER_RTT_printf(0, "[PARSER] bad header: 0x%02X 0x%02X\n", data[0], data[1]);
		return -2;
	}

	struct proto_frame frame;
	memset(&frame, 0, sizeof(frame));

	frame.ver     = data[2];
	frame.control = data[3];
	frame.cmdid   = data[4];
	frame.length  = ((uint16_t)data[5] << 8) | data[6];
	frame.recv_crc = ((uint16_t)data[7] << 8) | data[8];

	if (frame.length < PROTO_MIN_FRAME_LEN ||
	    frame.length > PROTO_MAX_FRAME_LEN) {
		SEGGER_RTT_printf(0, "[PARSER] bad length: %u\n", frame.length);
		return -1;
	}

	if (frame.length > len) {
		SEGGER_RTT_printf(0, "[PARSER] truncated: need %u, got %u\n", frame.length, len);
		return -1;
	}

	frame.data_len = frame.length - PROTO_MIN_FRAME_LEN;
	if (frame.data_len > 0) {
		memcpy(frame.data, &data[9], frame.data_len);
	}

	/* CRC 校验: prefix[7] = head(2)+ver(1)+ctrl(1)+cmdid(1)+length(2) */
	uint8_t prefix[7] = {
		PROTO_HEAD_HI, PROTO_HEAD_LO,
		frame.ver, frame.control, frame.cmdid,
		(uint8_t)(frame.length >> 8),
		(uint8_t)(frame.length & 0xFF)
	};

	crc16_xmodem_init();
	crc16_xmodem_append(prefix, sizeof(prefix));
	crc16_xmodem_append(frame.data, frame.data_len);
	uint16_t calc_crc = crc16_xmodem_end();

	if (calc_crc != frame.recv_crc) {
		SEGGER_RTT_printf(0, "[PARSER] CRC mismatch: calc=0x%04X recv=0x%04X\n",
			calc_crc, frame.recv_crc);
		return -3;
	}

	proto_handle_frame(&frame);
	return 0;
}

int proto_engine_init(void)
{
	crc16_xmodem_init();
	proto_handler_init();
	SEGGER_RTT_printf(0, "[INFO] Protocol engine initialized\n");
	return 0;
}
