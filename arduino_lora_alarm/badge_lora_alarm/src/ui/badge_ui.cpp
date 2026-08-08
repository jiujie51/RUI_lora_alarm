#include <stdint.h>
/*
 * Badge UI — 两段式确认 + 按键→告警映射 + 上行触发 + OLED 显示
 * 移植自 NCS badge_ui.c
 *
 * OLED 显示内容:
 *   短按: 电池电量
 *   长按: 告警确认提示
 *   组合键: 操作结果
 *   入网状态: 自动更新
 */
#include <Arduino.h>
#include "badge_ui.h"
#include "button_sm.h"
#include "../app/alarm_sm.h"
#include "../app/actuator_mgr.h"
#include "../app/power_mgr.h"
#include "../app/app_hal.h"
#include "../config/config_store.h"
#include "../proto/proto_internal.h"
#include "../ble/ble_badge_scan.h"
#include "../drv/gps_drv.h"
#include "../boards/badge/board.h"

#if OLED_ENABLE
#include "../drv/oled_drv.h"
#endif
#include "../drv/led_strip.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 确认时序 ── */
#define CONFIRM_WINDOW_MS   2000  /* LONG 后需继续按住 2s */
#define CANCEL_DISPLAY_MS   1500  /* "Cancelled" 显示持续时间 */
#define LCD_WAKE_TIMEOUT_MS  10000 /* OLED 自动熄屏超时 */

/* ── UI 状态 ── */
enum ui_state {
	UI_IDLE = 0,
	UI_CONFIRM_PENDING,
	UI_CANCELLED,
};

static enum ui_state ui_st = UI_IDLE;
static uint8_t  ui_confirm_btn;       /* 等待确认的按键 ID */
static uint8_t  ui_confirm_alarm;     /* 对应的告警类型 */
static uint32_t ui_confirm_start_ms;  /* PENDING 开始时间 */
static uint32_t ui_cancelled_ms;      /* CANCELLED 开始时间 */
static bool     device_enabled = true;

/* ── OLED 自动熄屏 ── */
#if OLED_ENABLE
static uint32_t ui_lcd_wake_ms;       /* OLED 最后唤醒时间 */

/* CMD 0x08/0x09 LCD content cache (for line2 show/hide toggle) */
static char lcd_line1_buf[17];        /* CMD 0x08 stored line1 */
static char lcd_line2_buf[17];        /* CMD 0x08 stored line2 */
static bool lcd_line2_visible;        /* CMD 0x09 line2 visibility flag */
#endif

/* ── 按键→告警映射 ── */
static const uint8_t btn_to_alarm[BTN_COUNT] = {
	ALARM_TYPE_RED,    /* BTN_RED    = 0 */
	ALARM_TYPE_BLUE,   /* BTN_BLUE   = 1 */
	ALARM_TYPE_YELLOW, /* BTN_YELLOW = 2 */
	ALARM_TYPE_GREEN,  /* BTN_GREEN  = 3 */
};

/* ── 告警名称数组 (OLED 显示, 按 alarm_type 索引, 对齐 NCS) ── */
#if OLED_ENABLE
static const char *alarm_names[] = {
	[ALARM_TYPE_RED - 1]      = "Code Red",
	[ALARM_TYPE_BLUE - 1]     = "Medical Alert",
	[ALARM_TYPE_YELLOW - 1]   = "Admin Alert",
	[ALARM_TYPE_GREEN - 1]    = "All Clear",
	[ALARM_TYPE_HOLD - 1]     = "HOLD ALERT",
	[ALARM_TYPE_SECURE - 1]   = "SECURE ALERT",
	[ALARM_TYPE_EVACUATE - 1] = "EVACUATE ALERT",
	[ALARM_TYPE_SHELTER - 1]  = "SHELTER ALERT",
};

/* ── OLED 辅助: 显示两行文字 ── */
static void oled_show_two_lines(const char *line1, const char *line2) {
	oled_clear_line(0);
	if (line1) oled_draw_string(0, 0, line1);
	oled_clear_line(2);
	if (line2) oled_draw_string(0, 2, line2);
}

/* ── 唤醒 OLED ── */
static void oled_wake(void) {
	oled_display_on();
	ui_lcd_wake_ms = millis();
}
#endif

/* ── 公开: 显示当前告警名称 (持久, 告警激活时防熄屏) ── */
void badge_ui_show_alarm(void) {
#if OLED_ENABLE
	uint8_t type = alarm_sm_current_type();
	if (type == 0) return;  /* 无告警, 不强制点亮 */

	const char *name = (type >= 1 && type <= 8)
		? alarm_names[type - 1] : "ALERT";
	oled_wake();

	/* Red/Blue/Yellow 第二行显示房间号 */
	if (type == ALARM_TYPE_RED || type == ALARM_TYPE_BLUE || type == ALARM_TYPE_YELLOW) {
		char room_line[17];
		snprintf(room_line, sizeof(room_line), "Room %d", config_get_room_id());
		oled_show_two_lines(name, room_line);
	} else {
		oled_show_two_lines(name, NULL);
	}
	SEGGER_RTT_printf(0, "[UI] OLED show alarm: %s (type=%d)\n", name, type);
#endif
}

/* ── 公开: CMD 0x08 LCD Content (固定 41 字节格式) ── */
void badge_ui_clear_display(void) {
#if OLED_ENABLE
	oled_clear();
#endif
}

void badge_ui_set_lcd_content(const char *line1, const char *line2) {
#if OLED_ENABLE
	if (line1) {
		strncpy(lcd_line1_buf, line1, 16);
		lcd_line1_buf[16] = '\0';
	} else {
		lcd_line1_buf[0] = '\0';
	}
	if (line2) {
		strncpy(lcd_line2_buf, line2, 16);
		lcd_line2_buf[16] = '\0';
	} else {
		lcd_line2_buf[0] = '\0';
	}
	lcd_line2_visible = true;

	oled_wake();
	oled_show_two_lines(lcd_line1_buf, lcd_line2_buf);
	SEGGER_RTT_printf(0, "[UI] LCD content: L1=\"%s\" L2=\"%s\"\n",
		lcd_line1_buf, lcd_line2_buf);
#endif
}

/* ── 公开: CMD 0x09 LCD Line2 显隐切换 ── */
void badge_ui_set_lcd_line2_visible(bool visible) {
#if OLED_ENABLE
	lcd_line2_visible = visible;
	if (visible) {
		oled_wake();
		oled_clear_line(2);
		if (lcd_line2_buf[0]) oled_draw_string(0, 2, lcd_line2_buf);
	} else {
		oled_clear_line(2);  /* 仅清除 line2, line1 保持 */
	}
	SEGGER_RTT_printf(0, "[UI] LCD line2: %s\n", visible ? "ON" : "OFF");
#endif
}

/* ── 告警名称 (日志用) ── */
static const char *alarm_names_log[BTN_COUNT] = { "CODE RED", "CODE BLUE", "CODE YELLOW", "ALL CLEAR" };

/* ── 辅助 ── */
static void ui_confirm_reset(void) {
	ui_st = UI_IDLE;
	ui_confirm_btn = 0;
	ui_confirm_alarm = 0;
}

static uint8_t  pending_uplink_btn = 0xFF;  /* 0xFF=无待发上行 */
static uint32_t pending_uplink_ms;           /* 上行请求时间 (看门狗用) */
#define UPLINK_WATCHDOG_MS  10000            /* BLE 扫描超时强制发送 (10s) */

/* ── 触发告警 + 启动 BLE 扫描 ── */
static void ui_trigger_alert(uint8_t alarm_type, uint8_t btn_id) {
	if (alarm_type == ALARM_TYPE_GREEN) {
		if (alarm_sm_current_priority() != ALARM_PRIO_CODE_RED) {
			SEGGER_RTT_printf(0, "[INFO] Green — no Code Red active, ignored\n");
#if OLED_ENABLE
			oled_show_two_lines("No alarm to clear", NULL);
#endif
			return;
		}
		alarm_sm_all_clear();
		SEGGER_RTT_printf(0, "[INFO] ALL CLEAR triggered by button %d\n", btn_id);
#if OLED_ENABLE
		oled_show_two_lines("ALL CLEAR", NULL);
#endif
	} else {
		alarm_sm_set(alarm_type, ALARM_SRC_BADGE_BTN);
		SEGGER_RTT_printf(0, "[INFO] Alarm type=%d triggered by button %d\n", alarm_type, btn_id);

#if OLED_ENABLE
		const char *name = (alarm_type >= 1 && alarm_type <= 4)
			? alarm_names[alarm_type - 1] : "ALERT";
		char line1[17];
		snprintf(line1, sizeof(line1), "%-16s", name);
		oled_show_two_lines(line1, "Sending alert...");
#endif
	}

	actuator_mgr_sync();
	badge_ui_show_alarm();  /* OLED 显示当前告警名称 */

	ble_scan_start_alert();
	pending_uplink_btn = btn_id;
	pending_uplink_ms  = millis();  /* watchdog 计时起点 */
}

/* ── 发送按键上行 (BLE 扫描完成后调用) ── */
static void send_key_event_uplink(uint8_t btn_id) {
	SEGGER_RTT_printf(0, "[UPLINK] step1: check joined/locked\n");
	if (!app_hal_is_joined()) return;
	// if (!app_hal_is_beacon_locked()) return;

	SEGGER_RTT_printf(0, "[UPLINK] step2: get location\n");
	const struct ble_scan_result *result = ble_scan_get_result();
	static uint8_t buf[64];
	int8_t  rssi    = 0;
	const uint8_t *hub_mac = (const uint8_t *)"\x00\x00\x00\x00\x00\x00";
	int32_t lat     = 0;
	int32_t lon     = 0;

	if (result->valid) {
		rssi    = result->rssi;
		hub_mac = result->hub_mac;
		SEGGER_RTT_printf(0, "[UPLINK] BLE hub: MAC=%02X:%02X RSSI=%d room=%d\n",
			hub_mac[0], hub_mac[1], hub_mac[2], hub_mac[3], hub_mac[4], hub_mac[5],
			rssi, result->room_id);
	} else {
		SEGGER_RTT_printf(0, "[UPLINK] BLE no hub, fallback to GPS...\n");
#if GPS_ENABLE
		if (gps_drv_has_fix()) {
			gps_drv_get_position(&lat, &lon);
			SEGGER_RTT_printf(0, "[UPLINK] GPS fix: lat=%d lon=%d sats=%d\n",
				lat, lon, gps_drv_satellites());
		} else
#endif
		{
			SEGGER_RTT_printf(0, "[UPLINK] GPS no fix either — sending without location\n");
		}
	}

	SEGGER_RTT_printf(0, "[UPLINK] step3: build frame\n");
	int len = proto_build_key_event(buf, sizeof(buf), btn_id, 0, rssi,
		hub_mac, lat, lon);

	SEGGER_RTT_printf(0, "[UPLINK] step4: len=%d, send...\n", len);
	if (len > 0) {
		app_hal_send(FPORT_COMMON, buf, len, false);
		SEGGER_RTT_printf(0, "[UPLINK] step5: send done\n");
	}
	SEGGER_RTT_printf(0, "[UPLINK] done\n");
}

/* ── 确认 tick (每 10ms, 从 badge_ui_poll 调用) ── */
static void badge_ui_confirm_tick(void) {
	uint32_t now = millis();

	switch (ui_st) {
	case UI_IDLE:
#if OLED_ENABLE
		/* OLED 自动熄屏 (IDLE 状态超时) */
		if (ui_lcd_wake_ms != 0 && (now - ui_lcd_wake_ms) >= LCD_WAKE_TIMEOUT_MS
			    && !alarm_sm_is_active()) {
			oled_display_off();
			ui_lcd_wake_ms = 0;
		}
#endif
		return;

	case UI_CONFIRM_PENDING: {
		/* 多键中止 */
		int pressed = 0;
		for (int i = 0; i < BTN_COUNT; i++)
			if (button_is_pressed(i)) pressed++;
		if (pressed >= 2) {
			SEGGER_RTT_printf(0, "[INFO] Confirmation aborted (multi-key: %d pressed)\n", pressed);
			ui_confirm_reset();
			return;
		}

		/* 确认窗口判断 */
		if (button_is_pressed(ui_confirm_btn)) {
			uint32_t elapsed = now - ui_confirm_start_ms;
			if (elapsed >= CONFIRM_WINDOW_MS) {
				SEGGER_RTT_printf(0, "[INFO] Btn[%d] %s confirmed after %lums\n",
					ui_confirm_btn, alarm_names_log[ui_confirm_btn], elapsed);
				ui_trigger_alert(ui_confirm_alarm, ui_confirm_btn);
				ui_confirm_reset();
			}
		} else {
			/* 松手 → 取消 */
			SEGGER_RTT_printf(0, "[INFO] Btn[%d] confirmation cancelled (released early)\n",
				ui_confirm_btn);
			ui_st = UI_CANCELLED;
			ui_cancelled_ms = now;
#if OLED_ENABLE
			oled_show_two_lines("Cancelled", NULL);
#endif
		}
		return;
	}

	case UI_CANCELLED:
		if ((now - ui_cancelled_ms) >= CANCEL_DISPLAY_MS) {
			ui_confirm_reset();
		}
		return;
	}
}

/* ── 按键事件回调 ── */
static void on_button_event(uint8_t id, enum btn_event evt) {
	/* COMBO 始终有效 */
	if (evt == BTN_EVENT_COMBO) {
		ui_confirm_reset();
		if (id == BTN_BLUE) {
				/* Blue+Yellow 5s: Reset - flash white + vibrate + LCD version (no buzzer) */
				SEGGER_RTT_printf(0, "[INFO] Combo: Reset\n");
				alarm_sm_clear_all();
				/* Flash white + vibrate briefly */
				struct led_color white = {255, 255, 255};
				led_strip_set_all(white);
				digitalWrite(MOTOR_PIN, HIGH);
				delay(500);
				led_strip_off();
				digitalWrite(MOTOR_PIN, LOW);
				actuator_mgr_sync();
#if OLED_ENABLE
				oled_wake();
				oled_show_two_lines("Firmware Version", "V1.0");
#endif
			} else if (id == BTN_GREEN) {
			/* Green+Blue 5s: Device disable/re-enable */
			device_enabled = !device_enabled;
			SEGGER_RTT_printf(0, "[INFO] Combo: Device %s\n", device_enabled ? "ENABLED" : "DISABLED");
#if OLED_ENABLE
			oled_wake();
			oled_show_two_lines(device_enabled ? "DEVICE ENABLED" : "DEVICE DISABLED", NULL);
#endif
		}
		return;
	}

	if (!device_enabled) {
		SEGGER_RTT_printf(0, "[INFO] Device disabled — button %d ignored\n", id);
		return;
	}

#if OLED_ENABLE
	oled_wake();
#endif

	switch (evt) {
	case BTN_EVENT_SHORT:
		ui_confirm_reset();
		{
			uint8_t pct = power_mgr_get_battery_pct();
			SEGGER_RTT_printf(0, "[INFO] Btn[%d] SHORT: battery %d%%\n", id, pct);
#if OLED_ENABLE
			char line1[17];
			snprintf(line1, sizeof(line1), "Battery: %d%%", pct);
			const char *line2 = power_mgr_is_charging() ? "Charging" : NULL;
			oled_show_two_lines(line1, line2);
#endif
		}
		break;

	case BTN_EVENT_LONG: {
		ui_confirm_reset();
		ui_st = UI_CONFIRM_PENDING;
		ui_confirm_btn = id;
		ui_confirm_alarm = btn_to_alarm[id];
		ui_confirm_start_ms = millis();
		SEGGER_RTT_printf(0, "[INFO] Btn[%d] LONG — Hold 2s to confirm [%s]\n",
			id, alarm_names_log[id]);
#if OLED_ENABLE
		{
			const char *name = (ui_confirm_alarm >= 1 && ui_confirm_alarm <= 4)
				? alarm_names[ui_confirm_alarm - 1] : "ALERT";
			char line1[17];
			snprintf(line1, sizeof(line1), "%-14s Hold 2s", name);
			oled_show_two_lines(line1, "to confirm...");
		}
#endif
		break;
	}

	default:
		break;
	}
}

/* ── 入网状态 (不在 OLED 上显示) ── */
void badge_ui_show_join_status(int state) { (void)state; }

/* ── 公共 API ── */
int badge_ui_init(void) {
	SEGGER_RTT_printf(0, "[UI] button_sm_init...\n");
	button_sm_init();
	SEGGER_RTT_printf(0, "[UI] button_sm_init OK\n");
	button_sm_set_callback(on_button_event);
	SEGGER_RTT_printf(0, "[UI] callback set\n");
	ui_confirm_reset();

	SEGGER_RTT_printf(0, "[INFO] Badge UI initialized (two-step confirm + OLED)\n");
	SEGGER_RTT_printf(0, "[UI] badge_ui_init done\n");
	return 0;
}

void badge_ui_poll(void) {
	button_sm_poll();
	badge_ui_confirm_tick();

	/* BLE 扫描完成后发送按键上行 (watchdog: 超时 10s 强制发送, 避免 BLE 异常卡死) */
	if (pending_uplink_btn != 0xFF) {
		bool scan_done = !ble_scan_active();
		bool timed_out = (millis() - pending_uplink_ms) >= UPLINK_WATCHDOG_MS;
		if (scan_done || timed_out) {
			if (timed_out && !scan_done) {
				SEGGER_RTT_printf(0, "[WARN] Uplink watchdog timeout — sending without BLE location\n");
			}
			SEGGER_RTT_printf(0, "[INFO] send key event uplink\n");
			send_key_event_uplink(pending_uplink_btn);
			pending_uplink_btn = 0xFF;
		}
	}
}
