/*
 * 告警状态机 — 优先级抢占核心逻辑
 * 移植自 NCS: k_uptime_get() → millis(), LOG_* → LOG_*/Serial
 *
 * 规则: Red(P0)不可抢占 | All Clear仅清Red | Clear All清全部 | Green重按→新Red
 */
#include <Arduino.h>
#include <string.h>
#include "alarm_sm.h"
#include "../debug_macros.h"

#define TAG "alarm_sm"
#define MAX_ACTIVE_ALARMS  8

/* ── 优先级查找表 ── */
static uint8_t type_to_prio[16] = {[0 ... 15] = ALARM_PRIO_MAX};

static void init_type_to_prio(void) {
	type_to_prio[ALARM_TYPE_RED]      = ALARM_PRIO_CODE_RED;
	type_to_prio[ALARM_TYPE_BLUE]     = ALARM_PRIO_CODE_BLUE;
	type_to_prio[ALARM_TYPE_YELLOW]   = ALARM_PRIO_CODE_YELLOW;
	type_to_prio[ALARM_TYPE_GREEN]    = ALARM_PRIO_ALL_CLEAR;
	type_to_prio[ALARM_TYPE_HOLD]     = ALARM_PRIO_HOLD;
	type_to_prio[ALARM_TYPE_SECURE]   = ALARM_PRIO_SECURE;
	type_to_prio[ALARM_TYPE_EVACUATE] = ALARM_PRIO_EVACUATE;
	type_to_prio[ALARM_TYPE_SHELTER]  = ALARM_PRIO_SHELTER;
}

/* ── 状态 ── */
static struct active_alarm active_alarms[MAX_ACTIVE_ALARMS];
static uint8_t active_count;

/* ── 内部函数 ── */
static int find_alarm(uint8_t alarm_type) {
	for (int i = 0; i < active_count; i++)
		if (active_alarms[i].type == alarm_type) return i;
	return -1;
}

static void remove_alarm_at(int idx) {
	if (idx < 0 || idx >= active_count) return;
	memmove(&active_alarms[idx], &active_alarms[idx + 1],
		(active_count - idx - 1) * sizeof(struct active_alarm));
	active_count--;
}

static void sort_by_priority(void) {
	for (int i = 0; i < (int)active_count - 1; i++)
		for (int j = i + 1; j < active_count; j++)
			if (active_alarms[i].priority > active_alarms[j].priority) {
				struct active_alarm tmp = active_alarms[i];
				active_alarms[i] = active_alarms[j];
				active_alarms[j] = tmp;
			}
}

/* ── 公共 API ── */
int alarm_sm_init(void) {
	init_type_to_prio();
	memset(active_alarms, 0, sizeof(active_alarms));
	active_count = 0;
	LOG_INFO(TAG, "Alarm state machine initialized");
	return 0;
}

static bool is_valid_alarm_type(uint8_t t) {
	return t == ALARM_TYPE_RED || t == ALARM_TYPE_BLUE || t == ALARM_TYPE_YELLOW ||
	       t == ALARM_TYPE_GREEN || t == ALARM_TYPE_HOLD || t == ALARM_TYPE_SECURE ||
	       t == ALARM_TYPE_EVACUATE || t == ALARM_TYPE_SHELTER;
}

int alarm_sm_set(uint8_t alarm_type, uint8_t source) {
	if (!is_valid_alarm_type(alarm_type)) {
		LOG_WARN(TAG, "Unknown alarm type: 0x%02X", alarm_type);
		return -22; /* -EINVAL */
	}

	uint8_t new_prio = alarm_type_to_priority(alarm_type);

	if (alarm_type == ALARM_TYPE_GREEN) {
		if (active_count > 0 && active_alarms[0].priority == ALARM_PRIO_CODE_RED) {
			LOG_INFO(TAG, "Green re-press while Red active — Red stays");
			return 0;
		}
	}

	if (active_count > 0 && active_alarms[0].priority == ALARM_PRIO_CODE_RED
	    && new_prio != ALARM_PRIO_CODE_RED) {
		LOG_INFO(TAG, "Red active — alarm type=%d recorded but output stays Red", alarm_type);
	}

	int existing = find_alarm(alarm_type);
	if (existing >= 0) {
		active_alarms[existing].source |= (1 << source);
		active_alarms[existing].set_time = millis();
		return 0;
	}

	if (active_count >= MAX_ACTIVE_ALARMS) {
		LOG_ERROR(TAG, "Too many active alarms (max %d)", MAX_ACTIVE_ALARMS);
		return -12; /* -ENOMEM */
	}

	active_alarms[active_count].type     = alarm_type;
	active_alarms[active_count].priority = new_prio;
	active_alarms[active_count].source   = 1 << source;
	active_alarms[active_count].set_time = millis();
	active_count++;

	sort_by_priority();

	LOG_INFO(TAG, "Alarm set: type=%d prio=%d src=%d (active=%d, top=%d)",
		alarm_type, new_prio, source, active_count,
		active_count > 0 ? active_alarms[0].priority : -1);
	return 0;
}

int alarm_sm_clear(uint8_t alarm_type) {
	if (alarm_type == 0xFF) return alarm_sm_clear_all();

	int idx = find_alarm(alarm_type);
	if (idx < 0) return -2; /* -ENOENT */

	LOG_INFO(TAG, "Alarm cleared: type=%d prio=%d",
		active_alarms[idx].type, active_alarms[idx].priority);
	remove_alarm_at(idx);
	return 0;
}

int alarm_sm_all_clear(void) {
	if (active_count == 0 || active_alarms[0].priority != ALARM_PRIO_CODE_RED)
		return -2;

	LOG_INFO(TAG, "All Clear: removing Code Red");
	remove_alarm_at(0);
	sort_by_priority();
	return 0;
}

int alarm_sm_clear_all(void) {
	memset(active_alarms, 0, sizeof(active_alarms));
	active_count = 0;
	LOG_INFO(TAG, "Clear All: all alarms removed");
	return 0;
}

uint8_t alarm_sm_current_priority(void) {
	if (active_count == 0) return ALARM_PRIO_NORMAL;
	return active_alarms[0].priority;
}

uint8_t alarm_sm_current_type(void) {
	if (active_count == 0) return 0;
	return active_alarms[0].type;
}

bool alarm_sm_is_active(void) { return active_count > 0; }

int alarm_sm_get_active(struct active_alarm *list, uint8_t max_len) {
	uint8_t n = (active_count < max_len) ? active_count : max_len;
	memcpy(list, active_alarms, n * sizeof(struct active_alarm));
	return n;
}

uint8_t alarm_type_to_priority(uint8_t alarm_type) {
	if (alarm_type < 16) return type_to_prio[alarm_type];
	return ALARM_PRIO_NORMAL;
}

uint8_t alarm_priority_to_type(uint8_t priority) {
	for (int i = 0; i < 16; i++)
		if (type_to_prio[i] == priority) return i;
	return 0;
}
