/*
 * 帧解析器 — 6 状态流式帧解析
 * 移植自 NCS: k_uptime_get() → millis(), LOG_* → LOG_Serial
 */
#include <Arduino.h>
#include <string.h>
#include "proto_internal.h"
#include "../debug_macros.h"

#define TAG "proto_parser"

static parser_state_t state = PARSE_IDLE;
static struct proto_frame rx_frame;
static uint8_t data_idx;
static uint32_t parse_start_ms;

/* header 收集缓冲区: ver(1)+ctrl(1)+cmdid(1)+length(2)+crc(2) = 7 bytes */
static uint8_t  hdr_buf[7];
static uint8_t  hdr_idx;

static void reset_parser(void)
{
	state = PARSE_IDLE;
	memset(&rx_frame, 0, sizeof(rx_frame));
	hdr_idx = 0;
	data_idx = 0;
	parse_start_ms = 0;
}

int proto_parser_feed(uint8_t byte)
{
	/* 超时检测：超过 5s 未完成自动重置 */
	if (state != PARSE_IDLE &&
	    (millis() - parse_start_ms) > PROTO_PARSE_TIMEOUT_MS) {
		LOG_WARN(TAG, "Parser timeout, resetting");
		reset_parser();
	}

	switch (state) {
	case PARSE_IDLE:
		if (byte == PROTO_HEAD_HI) {
			memset(&rx_frame, 0, sizeof(rx_frame));
			data_idx = 0;
			hdr_idx = 0;
			state = PARSE_HEAD;
		}
		break;

	case PARSE_HEAD:
		if (byte == PROTO_HEAD_LO) {
			state = PARSE_HEADER;
			parse_start_ms = millis();
		} else if (byte != PROTO_HEAD_HI) {
			state = PARSE_IDLE;
		}
		break;

	case PARSE_HEADER:
		hdr_buf[hdr_idx++] = byte;
		if (hdr_idx >= 7) {
			rx_frame.ver     = hdr_buf[0];
			rx_frame.control = hdr_buf[1];
			rx_frame.cmdid   = hdr_buf[2];
			rx_frame.length  = (hdr_buf[3] << 8) | hdr_buf[4];
			rx_frame.recv_crc = (hdr_buf[5] << 8) | hdr_buf[6];

			if (rx_frame.length < PROTO_MIN_FRAME_LEN ||
			    rx_frame.length > PROTO_MAX_FRAME_LEN) {
				LOG_WARN(TAG, "Invalid frame length: %d", rx_frame.length);
				state = PARSE_ERROR;
				break;
			}

			rx_frame.data_len = rx_frame.length - PROTO_MIN_FRAME_LEN;

			if (rx_frame.data_len == 0) {
				state = PARSE_DONE;
			} else {
				state = PARSE_DATA;
			}
			data_idx = 0;
			hdr_idx = 0;
		}
		break;

	case PARSE_DATA:
		rx_frame.data[data_idx++] = byte;
		if (data_idx >= rx_frame.data_len) {
			state = PARSE_DONE;
		}
		break;

	case PARSE_DONE:
	case PARSE_ERROR:
		break;
	}

	return (state == PARSE_DONE) ? 1 : 0;
}

int proto_parser_get_frame(struct proto_frame *frame)
{
	if (state != PARSE_DONE) return -11; /* -EAGAIN */

	uint8_t prefix[7] = {
		PROTO_HEAD_HI, PROTO_HEAD_LO,
		rx_frame.ver, rx_frame.control, rx_frame.cmdid,
		(rx_frame.length >> 8) & 0xFF,
		rx_frame.length & 0xFF
	};

	crc16_xmodem_init();
	crc16_xmodem_append(prefix, sizeof(prefix));
	crc16_xmodem_append(rx_frame.data, rx_frame.data_len);
	uint16_t calc_crc = crc16_xmodem_end();

	if (calc_crc != rx_frame.recv_crc) {
		LOG_WARN(TAG, "CRC mismatch: calc=0x%04X recv=0x%04X",
			calc_crc, rx_frame.recv_crc);
		state = PARSE_IDLE;
		return -74; /* -EBADMSG */
	}

	memcpy(frame, &rx_frame, sizeof(*frame));
	state = PARSE_IDLE;
	return 0;
}

void proto_parser_reset(void) { reset_parser(); }

int proto_engine_init(void)
{
	reset_parser();
	crc16_xmodem_init();
	LOG_INFO(TAG, "Protocol engine initialized");
	return 0;
}
