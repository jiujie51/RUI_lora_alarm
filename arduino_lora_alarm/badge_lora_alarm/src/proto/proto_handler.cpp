#include <stdint.h>
/*
 * 命令处理器 — 下行命令分发 (对齐协议文档 V1.4)
 * 移植自 NCS: 移除 Zephyr/logging/include, NRF_LOG → SEGGER_RTT_printf
 */
#include <Arduino.h>
#include <string.h>
#include "proto_internal.h"
#include "../boards/badge/board.h"
#include "../app/alarm_sm.h"
#include "../app/actuator_mgr.h"
#include "../drv/buzzer_pwm.h"
#include "../config/config_store.h"
#if OLED_ENABLE
#include "../drv/oled_drv.h"
#endif

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 协议 ↔ 内部 值翻译 ── */
uint8_t proto_alarm_to_internal(uint8_t proto_val) {
	/* 协议值 0-7 → 内部 ALARM_TYPE_* (按数组索引映射) */
	static const uint8_t map[8] = {
		ALARM_TYPE_GREEN, ALARM_TYPE_BLUE, ALARM_TYPE_YELLOW,
		ALARM_TYPE_RED,   ALARM_TYPE_HOLD, ALARM_TYPE_SECURE,
		ALARM_TYPE_EVACUATE, ALARM_TYPE_SHELTER,
	};
	return (proto_val < 8) ? map[proto_val] : 0xFF;
}

uint8_t internal_button_to_proto(uint8_t btn) {
	/* 协议定义: 0=绿,1=蓝,2=黄,3=红
	 * BTN_RED=0→3, BTN_BLUE=1→1, BTN_YELLOW=2→2, BTN_GREEN=3→0 */
	static const uint8_t map[4] = { 3, 1, 2, 0 };
	return (btn < 4) ? map[btn] : 0xFF;
}

static inline uint32_t read_u32_be(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] << 8)  |  p[3];
}

/* ── 设备身份 ── */
static uint8_t device_group_id = 0;
static uint8_t device_room_id  = DEVICE_ROOM_ID;

/* ── 多播匹配 ── */
static bool match_multicast(uint8_t cmd_group, uint8_t cmd_room,
			    uint8_t dev_group, uint8_t dev_room) {
	if (cmd_room != 0 && cmd_room != ROOM_ALL && cmd_room != dev_room)
		return false;
	if (cmd_group == GROUP_ALL) return true;
	if (cmd_group == GROUP_NONE) return false;
	return (dev_group & cmd_group) != 0;
}

/* ── CMD 0x03: Code ── */
static int handle_code(const uint8_t *data, uint8_t len) {
	if (len < 2) { SEGGER_RTT_printf(0, "[WARN] CMD 0x03: need >=2 bytes\n"); return -22; }

	uint8_t cmd_group   = data[0];
	uint8_t proto_alarm = data[1];
	uint8_t cmd_room    = (len >= 3) ? data[2] : 0;

	if (!match_multicast(cmd_group, cmd_room, device_group_id, device_room_id))
		return -13; /* -EACCES */

	uint8_t alarm_type = proto_alarm_to_internal(proto_alarm);
	if (alarm_type == 0xFF) {
		SEGGER_RTT_printf(0, "[WARN] CMD 0x03: unknown proto alarm %d\n", proto_alarm);
		return -22;
	}

	SEGGER_RTT_printf(0, "[INFO] CMD 0x03 Code: group=0x%02X alarm=%d -> internal=%d\n",
		cmd_group, proto_alarm, alarm_type);

	int ret = alarm_sm_set(alarm_type, ALARM_SRC_LORAWAN);
	if (ret == 0) actuator_mgr_sync();
	return ret;
}

/* ── CMD 0x04: Code Setting ── */
static int handle_code_setting(const uint8_t *data, uint8_t len) {
	if (len < 24) { SEGGER_RTT_printf(0, "[WARN] CMD 0x04: need 24 bytes\n"); return -22; }

	uint8_t cmd_group   = data[0];
	uint8_t proto_alarm = data[1];
	/* enable_sw = data[2]; */
	uint8_t mode_sw     = data[3];
	uint8_t volume      = data[4];
	uint32_t buzz_on    = read_u32_be(&data[5]);
	uint32_t buzz_off   = read_u32_be(&data[9]);
	uint8_t led_r       = data[21];
	uint8_t led_g       = data[22];
	uint8_t led_b       = data[23];

	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id))
		return -13;

	uint8_t alarm_type = proto_alarm_to_internal(proto_alarm);
	if (alarm_type == 0xFF) return -22;
	uint8_t prio = alarm_type_to_priority(alarm_type);
	if (prio >= ALARM_PRIO_MAX) return -22;

	struct alarm_config cfg;
	memcpy(&cfg, actuator_get_config(), sizeof(cfg));

	cfg.led_map[prio].r = led_r;
	cfg.led_map[prio].g = led_g;
	cfg.led_map[prio].b = led_b;
	cfg.led_map[prio].mode = (mode_sw & 0x01) ? 2 : 1;
	cfg.led_map[prio].on_ms = 0;
	cfg.led_map[prio].off_ms = 0;

	cfg.buzzer_map[prio].volume = (volume > 10) ? 10 : volume;
	if (mode_sw & 0x01) {
		cfg.buzzer_map[prio].mode   = BUZZER_PATTERN;
		cfg.buzzer_map[prio].on_ms  = (uint16_t)(buzz_on > 0xFFFF ? 0xFFFF : buzz_on);
		cfg.buzzer_map[prio].off_ms = (uint16_t)(buzz_off > 0xFFFF ? 0xFFFF : buzz_off);
	} else {
		cfg.buzzer_map[prio].mode = BUZZER_ON;
		cfg.buzzer_map[prio].on_ms = cfg.buzzer_map[prio].off_ms = 0;
	}

	actuator_set_config(&cfg);
	config_save_alarm_config(&cfg);

	SEGGER_RTT_printf(0, "[INFO] CMD 0x04: config updated+persisted alarm=%d prio=%d led=(%d,%d,%d) vol=%d\n",
		proto_alarm, prio, led_r, led_g, led_b, volume);
	return 0;
}

/* ── CMD 0x05: LED Control ── */
static int handle_led_control(const uint8_t *data, uint8_t len) {
	if (len < 15) { SEGGER_RTT_printf(0, "[WARN] CMD 0x05: need 15 bytes\n"); return -22; }

	uint8_t cmd_group = data[0];
	uint8_t mode_sw   = data[2];
	uint8_t onoff     = data[3];
	uint32_t led_on   = read_u32_be(&data[4]);
	uint32_t led_off  = read_u32_be(&data[8]);
	uint8_t led_r     = data[12];
	uint8_t led_g     = data[13];
	uint8_t led_b     = data[14];

	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id)) return -13;

	uint8_t mode;
	uint16_t on_ms = 0, off_ms = 0;
	if (mode_sw & 0x01) {
		mode = 2; on_ms = (uint16_t)(led_on > 0xFFFF ? 0xFFFF : led_on);
		off_ms = (uint16_t)(led_off > 0xFFFF ? 0xFFFF : led_off);
	} else {
		mode = onoff ? 1 : 0;
	}

	return actuator_led_override(led_r, led_g, led_b, mode, on_ms, off_ms);
}

/* ── CMD 0x06: Buzzer Control ── */
static int handle_buzzer_control(const uint8_t *data, uint8_t len) {
	if (len < 12) { SEGGER_RTT_printf(0, "[WARN] CMD 0x06: need 12 bytes\n"); return -22; }

	uint8_t cmd_group = data[0];
	uint8_t mode_sw   = data[1];
	uint8_t onoff     = data[2];
	uint32_t buzz_on  = read_u32_be(&data[3]);
	uint32_t buzz_off = read_u32_be(&data[7]);
	uint8_t volume    = data[11];

	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id)) return -13;

	uint8_t mode; uint16_t on_ms = 0, off_ms = 0;
	if (mode_sw & 0x01) {
		mode = BUZZER_PATTERN;
		on_ms = (uint16_t)(buzz_on > 0xFFFF ? 0xFFFF : buzz_on);
		off_ms = (uint16_t)(buzz_off > 0xFFFF ? 0xFFFF : buzz_off);
	} else {
		mode = onoff ? BUZZER_ON : BUZZER_OFF;
	}

	return actuator_buzzer_override(mode, volume, on_ms, off_ms);
}

/* ── CMD 0x07: Vibration Control ──
 * 协议: group(1) + mode_sw(1) + onoff(1) + action_type(1) + vib_on(4) + vib_off(4) = 12 bytes */
static int handle_vibration_control(const uint8_t *data, uint8_t len) {
	if (len < 12) { SEGGER_RTT_printf(0, "[WARN] CMD 0x07: need 12 bytes\n"); return -22; }

	uint8_t cmd_group    = data[0];
	uint8_t mode_sw      = data[1];
	uint8_t onoff        = data[2];
	/* action_type = data[3]; — 0=动作, 1=设置 (暂不区分) */
	uint32_t vib_on      = read_u32_be(&data[4]);
	uint32_t vib_off     = read_u32_be(&data[8]);

	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id)) return -13;

	if (mode_sw & 0x01) {
		/* 间歇模式: 震动 on/off 循环 */
		actuator_vibration_override(2, (uint16_t)vib_on, (uint16_t)vib_off);
	} else {
		/* 常震模式 */
		actuator_vibration_override(onoff ? 1 : 0, 0, 0);
	}

	SEGGER_RTT_printf(0, "[INFO] CMD 0x07: vibration mode=%d onoff=%d\n", mode_sw, onoff);
	return 0;
}

/* ── CMD 0x08: LCD Content ──
 * 格式: group(1) + line1_len(1) + line1(line1_len) + line2_len(1) + line2(line2_len)
 * OLED: 128x64, 8x16 字体, 每行 16 字符 */
static int handle_lcd_content(const uint8_t *data, uint8_t len) {
	if (len < 3) { SEGGER_RTT_printf(0, "[WARN] CMD 0x08: need >=3 bytes\n"); return -22; }

	uint8_t cmd_group = data[0];
	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id))
		return -13;

	uint8_t line1_len = data[1];
	if (line1_len > 16) line1_len = 16;
	uint8_t pos = 2;

#if OLED_ENABLE
	char line1[17] = {0};
	if (line1_len > 0 && pos + line1_len <= len) {
		memcpy(line1, &data[pos], line1_len);
		pos += line1_len;
	}

	uint8_t line2_len = (pos < len) ? data[pos] : 0;
	pos++;
	char line2[17] = {0};
	if (line2_len > 16) line2_len = 16;
	if (line2_len > 0 && pos + line2_len <= len) {
		memcpy(line2, &data[pos], line2_len);
	}

	oled_clear_line(0);
	oled_draw_string(0, 0, line1);
	oled_clear_line(2);
	if (line2_len > 0) oled_draw_string(0, 2, line2);

	SEGGER_RTT_printf(0, "[INFO] CMD 0x08 LCD: L1=\"%s\" L2=\"%s\"\n", line1, line2);
#else
	SEGGER_RTT_printf(0, "[INFO] CMD 0x08 LCD: OLED disabled\n");
#endif
	return 0;
}

/* ── CMD 0x09: LCD Line2 On/Off ──
 * 格式: group(1) + enable(1) */
static int handle_lcd_line2_onoff(const uint8_t *data, uint8_t len) {
	if (len < 2) { SEGGER_RTT_printf(0, "[WARN] CMD 0x09: need 2 bytes\n"); return -22; }

	uint8_t cmd_group = data[0];
	uint8_t enable    = data[1];

	if (!match_multicast(cmd_group, 0, device_group_id, device_room_id))
		return -13;

#if OLED_ENABLE
	if (enable) {
		oled_display_on();
	} else {
		/* 仅关 line2 (清空 page 2-3), line1 保持 */
		oled_clear_line(2);
	}
	SEGGER_RTT_printf(0, "[INFO] CMD 0x09 LCD Line2: %s\n", enable ? "ON" : "OFF");
#else
	SEGGER_RTT_printf(0, "[INFO] CMD 0x09 LCD Line2: OLED disabled\n");
#endif
	return 0;
}

/* ── CMD 0x0A: Clear Packet ── */
static int handle_clear_packet(const uint8_t *data, uint8_t len) {
	if (len < 2) { SEGGER_RTT_printf(0, "[WARN] CMD 0x0A: need 2 bytes\n"); return -22; }

	uint8_t cmd_group  = data[0];
	uint8_t clear_type = data[1];
	uint8_t cmd_room   = (len >= 3) ? data[2] : 0;

	if (!match_multicast(cmd_group, cmd_room, device_group_id, device_room_id))
		return -13;

	if (clear_type == 0) {
		SEGGER_RTT_printf(0, "[INFO] CMD 0x0A: Clear All\n");
		alarm_sm_clear_all();
	} else if (clear_type == 1) {
		SEGGER_RTT_printf(0, "[INFO] CMD 0x0A: All Clear (Code Red only)\n");
		alarm_sm_all_clear();
	} else {
		return -22;
	}

	actuator_mgr_sync();
	return 0;
}

/* ── CMD 0x50: Set Group ID ── */
static int handle_set_group_id(const uint8_t *data, uint8_t len) {
	if (len < 1) { SEGGER_RTT_printf(0, "[WARN] CMD 0x50: need >=1 byte\n"); return -22; }

	uint8_t new_group = data[0];
	SEGGER_RTT_printf(0, "[INFO] CMD 0x50: Set Group ID: 0x%02X -> 0x%02X\n",
		device_group_id, new_group);

	device_group_id = new_group;
	config_set_group_id(new_group);
	return 0;
}

/* ── 命令分发表 (运行时初始化, C++14 兼容) ── */
cmd_handler_t cmd_handlers[256];

void proto_handler_init(void) {
	memset(cmd_handlers, 0, sizeof(cmd_handlers));
	cmd_handlers[CMDID_CODE]               = handle_code;
	cmd_handlers[CMDID_CODE_SETTING]       = handle_code_setting;
	cmd_handlers[CMDID_LED_CONTROL]        = handle_led_control;
	cmd_handlers[CMDID_BUZZER_CONTROL]     = handle_buzzer_control;
	cmd_handlers[CMDID_VIBRATION_CONTROL]  = handle_vibration_control;
	cmd_handlers[CMDID_LCD_CONTENT]        = handle_lcd_content;
	cmd_handlers[CMDID_LCD_LINE2_ONOFF]    = handle_lcd_line2_onoff;
	cmd_handlers[CMDID_CLEAR_PACKET]       = handle_clear_packet;
	cmd_handlers[CMDID_SET_GROUP_ID]       = handle_set_group_id;
}

int proto_handle_frame(const struct proto_frame *frame) {
	if (frame->cmdid >= 256) return -22;
	cmd_handler_t handler = cmd_handlers[frame->cmdid];
	if (handler == NULL) return -95; /* -ENOTSUP */
	return handler(frame->data, frame->data_len);
}

uint8_t proto_get_group_id(void) { return device_group_id; }
uint8_t proto_get_room_id(void)  { return device_room_id; }
