#include <stdint.h>
/*
 * 电源管理 — RUI3 版
 * 电池分压: P0.02/AIN0 ← R5(10K)→VBAT, R4(10K)→GND, 1/2 分压
 * 用 api.system.bat.get() 读取电压 (float), 无需手动 ADC
 */
#include <Arduino.h>
#include "power_mgr.h"
#include "../boards/hub/board.h"
#include "nrf_log.h"


static uint16_t battery_mv;
static uint8_t  battery_pct = 100;
static enum power_mode current_mode = POWER_NORMAL;

static uint8_t voltage_to_pct(uint16_t mv) {
	if (mv >= BATTERY_FULL_MV) return 100;
	if (mv <= BATTERY_EMPTY_MV) return 0;
	return (uint8_t)((uint32_t)(mv - BATTERY_EMPTY_MV) * 100 /
		(BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

int power_mgr_init(void) {
	power_mgr_update();
	NRF_LOG_INFO("Power manager initialized: %dmV (%d%%)", battery_mv, battery_pct);
	return 0;
}

void power_mgr_update(void) {
	float v = api.system.bat.get();
	battery_mv = (uint16_t)(v * 1000.0f);
	battery_pct = voltage_to_pct(battery_mv);

	if (battery_pct < BATTERY_LOW_PCT)
		current_mode = POWER_LOW_BATT;
	else
		current_mode = POWER_NORMAL;
}

uint16_t power_mgr_get_voltage_mv(void) { return battery_mv; }
uint8_t  power_mgr_get_battery_pct(void) { return battery_pct; }
enum power_mode power_mgr_get_mode(void) { return current_mode; }
bool power_mgr_is_charging(void) { return false; }
bool power_mgr_is_low_battery(void) { return battery_pct < BATTERY_LOW_PCT; }
