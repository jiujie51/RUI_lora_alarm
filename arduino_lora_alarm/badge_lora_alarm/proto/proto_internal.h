/*
 * 协议内部头文件 — parser / builder / handler 共享定义
 * 移植自 NCS: 移除 <zephyr/kernel.h>, 添加 <Arduino.h> / <stdint.h>
 */

#ifndef PROTO_INTERNAL_H
#define PROTO_INTERNAL_H

#include <Arduino.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── 帧常量 ── */
#define PROTO_HEAD_HI        0xAA
#define PROTO_HEAD_LO        0x55
#define PROTO_VER            0x01
#define PROTO_MIN_FRAME_LEN  9
#define PROTO_MAX_FRAME_LEN  255
#define PROTO_RX_BUF_SIZE    512
#define PROTO_PARSE_TIMEOUT_MS  5000

/* ── CMDID ── */
#define CMDID_HEARTBEAT      0x00
#define CMDID_POWER          0x01
#define CMDID_KEY_EVENT      0x02
#define CMDID_CODE           0x03
#define CMDID_CODE_SETTING   0x04
#define CMDID_LED_CONTROL    0x05
#define CMDID_BUZZER_CONTROL 0x06
#define CMDID_VIBRATION_CONTROL  0x07
#define CMDID_LCD_CONTENT    0x08
#define CMDID_LCD_LINE2_ONOFF    0x09
#define CMDID_CLEAR_PACKET   0x0A
#define CMDID_SET_GROUP_ID   0x50

/* ── Device type ── */
#define DEV_TYPE_BADGE       0x00
#define DEV_TYPE_HUB         0x01

/* ── Group ID (bitmask) ── */
#define GROUP_ADMIN          0x01
#define GROUP_NURSE          0x02
#define GROUP_SECURE         0x04
#define GROUP_PRINCIPAL      0x08
#define GROUP_ALL            0x80
#define GROUP_NONE           0x00

/* ── Room ID ── */
#define ROOM_ALL             0xFF

/* ── FPort 分配 ── */
#define FPORT_BADGE_UP       10
#define FPORT_HUB_UP         11
#define FPORT_COMMON          20

/* ── CRC16/XMODEM ── */
void crc16_xmodem_init(void);
void crc16_xmodem_append(const uint8_t *buf, uint16_t len);
uint16_t crc16_xmodem_end(void);

/* ── 帧解析器 ── */
typedef enum {
	PARSE_IDLE,
	PARSE_HEAD,
	PARSE_HEADER,
	PARSE_DATA,
	PARSE_DONE,
	PARSE_ERROR,
} parser_state_t;

struct proto_frame {
	uint8_t  ver;
	uint8_t  control;
	uint8_t  cmdid;
	uint16_t length;
	uint16_t recv_crc;
	uint8_t  data[PROTO_MAX_FRAME_LEN - PROTO_MIN_FRAME_LEN];
	uint8_t  data_len;
};

int proto_parser_feed(uint8_t byte);
int proto_parser_get_frame(struct proto_frame *frame);
void proto_parser_reset(void);

/* ── 帧构建器 ── */
int proto_build_heartbeat(uint8_t *buf, uint16_t buf_len,
			  uint8_t dev_type, uint8_t group_id);
int proto_build_power(uint8_t *buf, uint16_t buf_len,
		      uint8_t dev_type, uint8_t power_pct);
int proto_build_key_event(uint8_t *buf, uint16_t buf_len,
			  uint8_t button, uint8_t motion, int8_t rssi,
			  const uint8_t hub_mac[6],
			  int32_t lat, int32_t lon);

/* ── 命令处理器 ── */
typedef int (*cmd_handler_t)(const uint8_t *data, uint8_t len);
extern const cmd_handler_t cmd_handlers[256];
int proto_handle_frame(const struct proto_frame *frame);

/* ── 模块初始化 ── */
int proto_engine_init(void);

/* ── 运行时 Group ID / Room ID ── */
uint8_t proto_get_group_id(void);
uint8_t proto_get_room_id(void);

/* ── 协议 ↔ 内部翻译 ── */
uint8_t proto_alarm_to_internal(uint8_t proto_val);
uint8_t internal_button_to_proto(uint8_t btn);

#endif /* PROTO_INTERNAL_H */
