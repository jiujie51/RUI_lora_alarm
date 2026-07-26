#include <stdint.h>
/*
 * Badge UI — 两段式确认 + 按键→告警映射 + 上行触发
 * 移植自 NCS badge_ui.c
 *
 * 简化: 无 OLED, 无 GPS, 无设备睡眠
 * 确认: 长按 3s → "Hold 2s" → 继续 2s → 触发
 *       松手取消 | 第二键按下中止
 * 组合: Yellow+Green 5s → Clear All
 *       Green+Blue 5s → Device toggle
 */
#include <Arduino.h>
#include "badge_ui.h"
#include "button_sm.h"
#include "../app/alarm_sm.h"
#include "../app/actuator_mgr.h"
#include "../app/power_mgr.h"
#include "../app/app_hal.h"
#include "../proto/proto_internal.h"
#include "../ble/ble_badge_scan.h"
#include "../drv/gps_drv.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 确认时序 ── */
#define CONFIRM_WINDOW_MS   2000  /* LONG 后需继续按住 2s */
#define CANCEL_DISPLAY_MS   1500  /* "Cancelled" 显示持续时间 */

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

/* ── 按键→告警映射 ── */
static const uint8_t btn_to_alarm[BTN_COUNT] = {
	ALARM_TYPE_RED,    /* BTN_RED    = 0 */
	ALARM_TYPE_BLUE,   /* BTN_BLUE   = 1 */
	ALARM_TYPE_YELLOW, /* BTN_YELLOW = 2 */
	ALARM_TYPE_GREEN,  /* BTN_GREEN  = 3 */
};

/* ── 告警名称 (日志用) ── */
static const char *alarm_names[BTN_COUNT] = { "CODE RED", "CODE BLUE", "CODE YELLOW", "ALL CLEAR" };

/* ── 辅助 ── */
static void ui_confirm_reset(void) {
	ui_st = UI_IDLE;
	ui_confirm_btn = 0;
	ui_confirm_alarm = 0;
}

static uint8_t pending_uplink_btn = 0xFF;  /* 0xFF=无待发上行 */

/* ── 触发告警 + 启动 BLE 扫描 ── */
static void ui_trigger_alert(uint8_t alarm_type, uint8_t btn_id) {
	if (alarm_type == ALARM_TYPE_GREEN) {
		if (alarm_sm_current_priority() != ALARM_PRIO_CODE_RED) {
			SEGGER_RTT_printf(0, "[INFO] Green — no Code Red active, ignored\n");
			return;
		}
		alarm_sm_all_clear();
		SEGGER_RTT_printf(0, "[INFO] ALL CLEAR triggered by button %d\n", btn_id);
	} else {
		alarm_sm_set(alarm_type, ALARM_SRC_BADGE_BTN);
		SEGGER_RTT_printf(0, "[INFO] Alarm type=%d triggered by button %d\n", alarm_type, btn_id);
	}

	actuator_mgr_sync();

	/* 仅当 Class B beacon 锁定时才启动 BLE 扫描 + 上行
	 * - 无 beacon lock: 上行会被 app_hal_send 阻塞, BLE 扫描无意义
	 * - BLE 扫描可能干扰信标接收, 只在信标已锁定时使用
	 */
	// if (app_hal_is_beacon_locked()) {
		ble_scan_start_alert();
		pending_uplink_btn = btn_id;
	// } else {
	// 	SEGGER_RTT_printf(0, "[INFO] Beacon not locked, skip BLE scan and uplink\n");
	// }
}

/* ── 发送按键上行 (BLE 扫描完成后调用) ── */
static void send_key_event_uplink(uint8_t btn_id) {
	SEGGER_RTT_printf(0, "[UPLINK] step1: check joined/locked\n");
	if (!app_hal_is_joined()) return;
	if (!app_hal_is_beacon_locked()) return;

	SEGGER_RTT_printf(0, "[UPLINK] step2: get location\n");
	const struct ble_scan_result *result = ble_scan_get_result();
	static uint8_t buf[64];
	int8_t  rssi    = 0;
	const uint8_t *hub_mac = (const uint8_t *)"\x00\x00\x00\x00\x00\x00";
	int32_t lat     = 0;
	int32_t lon     = 0;

	if (result->valid) {
		/* BLE 扫描成功 → 用 Hub MAC 定位 */
		rssi    = result->rssi;
		hub_mac = result->hub_mac;
		SEGGER_RTT_printf(0, "[UPLINK] BLE hub: MAC=%02X:%02X RSSI=%d room=%d\n",
			hub_mac[0], hub_mac[1], hub_mac[2], hub_mac[3], hub_mac[4], hub_mac[5],
			rssi, result->room_id);
	} else {
		/* BLE 扫描失败 → 后备 GPS */
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
					ui_confirm_btn, alarm_names[ui_confirm_btn], elapsed);
				ui_trigger_alert(ui_confirm_alarm, ui_confirm_btn);
				ui_confirm_reset();
			}
		} else {
			/* 松手 → 取消 */
			SEGGER_RTT_printf(0, "[INFO] Btn[%d] confirmation cancelled (released early)\n",
				ui_confirm_btn);
			ui_st = UI_CANCELLED;
			ui_cancelled_ms = now;
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
			/* Blue+Yellow 5s: Reset — 清除所有告警, 关灯关蜂鸣 */
			SEGGER_RTT_printf(0, "[INFO] Combo: Reset (Clear All alarms)\n");
			alarm_sm_clear_all();
			actuator_all_off();
		} else if (id == BTN_GREEN) {
			/* Green+Blue 5s: Device disable/re-enable */
			device_enabled = !device_enabled;
			SEGGER_RTT_printf(0, "[INFO] Combo: Device %s\n", device_enabled ? "ENABLED" : "DISABLED");
		}
		return;
	}

	if (!device_enabled) {
		SEGGER_RTT_printf(0, "[INFO] Device disabled — button %d ignored\n", id);
		return;
	}

	switch (evt) {
	case BTN_EVENT_SHORT:
		ui_confirm_reset();
		SEGGER_RTT_printf(0, "[INFO] Btn[%d] SHORT: battery %d%%\n", id, power_mgr_get_battery_pct());
		break;

	case BTN_EVENT_LONG: {
		ui_confirm_reset();
		ui_st = UI_CONFIRM_PENDING;
		ui_confirm_btn = id;
		ui_confirm_alarm = btn_to_alarm[id];
		ui_confirm_start_ms = millis();
		SEGGER_RTT_printf(0, "[INFO] Btn[%d] LONG — Hold 2s to confirm [%s]\n",
			id, alarm_names[id]);
		break;
	}

	default:
		break;
	}
}

/* ── 公共 API ── */
int badge_ui_init(void) {
	SEGGER_RTT_printf(0, "[UI] button_sm_init...\n");
	button_sm_init();
	SEGGER_RTT_printf(0, "[UI] button_sm_init OK\n");
	button_sm_set_callback(on_button_event);
	SEGGER_RTT_printf(0, "[UI] callback set\n");
	ui_confirm_reset();
	SEGGER_RTT_printf(0, "[INFO] Badge UI initialized (two-step confirm)\n");
	SEGGER_RTT_printf(0, "[UI] badge_ui_init done\n");
	return 0;
}

void badge_ui_poll(void) {
	button_sm_poll();
	badge_ui_confirm_tick();

	/* BLE 扫描完成后发送按键上行 */
	if (pending_uplink_btn != 0xFF && !ble_scan_active()) {
		SEGGER_RTT_printf(0, "[INFO] send key event uplink\n");
		send_key_event_uplink(pending_uplink_btn);
		pending_uplink_btn = 0xFF;
	}
}
