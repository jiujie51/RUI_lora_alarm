#include <stdint.h>
/*
 * 电源管理 — RUI3 analogRead() 直接读取版
 *
 * RAK4630 底板电池分压: P0.02 (A2) ← R5(10K)→VBAT, R4(10K)→GND, 1/2 分压
 * RUI3 的 api.system.bat.get() 底层 BoardGetBatteryLevel() 为空函数 (返回 0)
 * 因此必须用 analogRead() 直接读 ADC.
 *
 * nRF52840 ADC: 14-bit, 参考电压 3.6V (内部 0.6V ref × 增益 6)
 * 实际测量范围 0-3.6V, analogReadResolution(14) → 0-16383
 */
#include <Arduino.h>
#include "power_mgr.h"
#include "../boards/hub/board.h"
extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);


static uint16_t battery_mv;
static uint8_t  battery_pct = 100;
static enum power_mode current_mode = POWER_NORMAL;

/* ── ADC 读取 (带简单滑动平均去噪) ── */
static uint16_t read_battery_mv(void) {
	/* 硬件分压 1/2, 所以实际电池电压 = ADC 读数 × 2 */
	analogReadResolution(14);
	uint32_t sum = 0;
	for (int i = 0; i < 8; i++) {
		sum += analogRead(BATTERY_ADC_PIN);
		delay(1);
	}
	uint16_t adc_val = (uint16_t)(sum / 8);
	/* ADC 14-bit (0-16383) → 电压 (0-3.6V) → 分压 ×2 → mV */
	uint16_t mv = (uint32_t)adc_val * 3600 * 2 / 16383;
	SEGGER_RTT_printf(0, "[PWR] ADC=%u (raw sum=%lu) → %umV\r\n", adc_val, sum, mv);
	return mv;
}

static uint8_t voltage_to_pct(uint16_t mv) {
	if (mv >= BATTERY_FULL_MV) return 100;
	if (mv <= BATTERY_EMPTY_MV) return 0;
	return (uint8_t)((uint32_t)(mv - BATTERY_EMPTY_MV) * 100 /
		(BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

int power_mgr_init(void) {
	analogReadResolution(14);
	pinMode(BATTERY_ADC_PIN, INPUT);
	power_mgr_update();
	SEGGER_RTT_printf(0, "[INFO] Power manager: %dmV (%d%%)\r\n", battery_mv, battery_pct);
	return 0;
}

void power_mgr_update(void) {
	battery_mv  = read_battery_mv();
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
