/*
 * @Author: jiefengzhu focus_feng@163.com
 * @Date: 2026-07-26 21:51:00
 * @LastEditors: jiefengzhu focus_feng@163.com
 * @LastEditTime: 2026-08-01 23:00:20
 * @FilePath: \RUI_lora_alarm\arduino_lora_alarm\badge_lora_alarm\src\drv\gps_drv.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <stdint.h>
/*
 * RAK12501 GPS 驱动 — TinyGPSPlus + Serial1 polling
 * 参考: examples/RAK12501/RAK12501_GPS_L76K.ino
 */
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "gps_drv.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

#define GPS_STALE_MS  30000   /* 30s 无更新 → 过期 */

static TinyGPSPlus gps;
static uint32_t last_fix_ms;
static bool     has_fix;

/* ══════════════════════════════════════════════════════════
 * 公共 API
 * ══════════════════════════════════════════════════════════ */

int gps_drv_init(void) {
	/* 复位 RAK12501 (WisBlock 槽位已供电, 只需复位脉冲) */
	pinMode(GPS_RESET_PIN, OUTPUT);
	digitalWrite(GPS_RESET_PIN, HIGH);
	delay(10);
	digitalWrite(GPS_RESET_PIN, LOW);
	delay(50);
	digitalWrite(GPS_RESET_PIN, HIGH);
	delay(100);

	/* UART: GPS_UART (Serial1) @ 9600bps */
	GPS_UART.begin(GPS_BAUDRATE);
	while (!GPS_UART);

	has_fix     = false;
	last_fix_ms = 0;

	SEGGER_RTT_printf(0, "[INFO] GPS initialized (TinyGPSPlus, RAK12501, %d baud)\n", GPS_BAUDRATE);
	return 0;
}

void gps_drv_poll(void) {
	while (GPS_UART.available() > 0) {
		if (gps.encode((char)GPS_UART.read())) {
			if (gps.location.isValid()) {
				has_fix     = true;
				last_fix_ms = millis();
			}
		}
	}
}

bool gps_drv_has_fix(void) {
	return has_fix && gps.location.isValid();
}

int gps_drv_get_position(int32_t *lat, int32_t *lon) {
	if (!gps_drv_has_fix()) {
		*lat = 90000001;
		*lon = 180000001;
		return -2;
	}
	*lat = (int32_t)(gps.location.lat() * 1e6);
	*lon = (int32_t)(gps.location.lng() * 1e6);
	return 0;
}

uint8_t gps_drv_satellites(void) {
	return gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;
}

bool gps_drv_is_stale(void) {
	if (!has_fix) return true;
	return (millis() - last_fix_ms) > GPS_STALE_MS;
}

void gps_drv_check_timeout(void) {
	if (gps_drv_is_stale()) {
		has_fix = false;
	}
}
