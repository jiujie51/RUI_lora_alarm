#include <stdint.h>
/*
 * Hub BLE 广播 — RUI3 api.ble.beacon.custom.payload
 *
 * 广播包 (31 bytes):
 *   [Flags: 0x02,0x01,0x06] [Complete Name: "ALARM_HUB"]
 *   [Manufacturer Data: MAC(6) + dev_type(1) + room_id(1)]
 *
 * 参考: ncs_lora_alarm/ble/ble_hub_adv.c
 * RUI3 示例: Example/BLE_Beacon_Custom_Payload/BLE_Beacon_Custom_Payload.ino
 */
#include <Arduino.h>
#include "ble_hub_adv.h"
#include "../proto/proto_internal.h"
#include "../boards/hub/board.h"
#include "nrf_log.h"

/* SEGGER_RTT 直写 — 绕过 NRF_LOG backend */
extern "C" {
int SEGGER_RTT_printf(unsigned BufferIndex, const char * sFormat, ...);
}


static uint8_t adv_payload[31];
static uint8_t adv_len = 0;

/* ── 从 BLE MAC 字符串解析 6 字节 ──
 * api.ble.mac.get(void) 返回 12 字符 hex 串 "XXXXXXXXXXXX" (无分隔符)
 * api.ble.mac.get(pos)  返回第 pos 字节的 2 字符 hex
 */
static void get_ble_mac(uint8_t mac[6]) {
	char *mac_str = api.ble.mac.get();
	if (mac_str == NULL || strlen(mac_str) < 12) {
		SEGGER_RTT_printf(0, "BLE: MAC get failed (len=%d)\n",
			mac_str ? (int)strlen(mac_str) : -1);
		memset(mac, 0, 6);
		return;
	}
	SEGGER_RTT_printf(0, "BLE: MAC = %s\n", mac_str);
	for (int i = 0; i < 6; i++) {
		char hex[3] = {mac_str[i * 2], mac_str[i * 2 + 1], '\0'};
		mac[i] = (uint8_t)strtoul(hex, NULL, 16);
	}
}

static void build_adv_data(void) {
	uint8_t *p = adv_payload;

	/* AD Flags (3 bytes) */
	*p++ = 0x02; *p++ = 0x01; *p++ = 0x06;

	/* Complete Local Name: "ALARM_HUB" (9 chars) */
	const char *name = BLE_ADV_NAME;
	uint8_t name_len = strlen(name);
	*p++ = name_len + 1;
	*p++ = 0x09;  /* AD Type: Complete Local Name */
	memcpy(p, name, name_len); p += name_len;

	/* Manufacturer Specific Data (2B company + 6B MAC + 1B type + 1B room) */
	uint8_t mfg_len = 2 + 6 + 1 + 1;
	*p++ = mfg_len + 1;
	*p++ = 0xFF;  /* AD Type */
	*p++ = 0xAC;  /* Company ID low  (0x04AC = RAKwireless) */
	*p++ = 0x04;  /* Company ID high */

	/* BLE MAC address (6 bytes) */
	uint8_t mac[6];
	get_ble_mac(mac);
	memcpy(p, mac, 6); p += 6;

	/* Device type + Room ID */
	*p++ = DEV_TYPE_HUB;
	*p++ = DEVICE_ROOM_ID;

	adv_len = p - adv_payload;
}

int ble_hub_adv_start(void) {
	SEGGER_RTT_printf(0, "BLE: switching to beacon mode...\n");

	api.ble.settings.blemode(RAK_BLE_BEACON_MODE);
	delay(500);

	build_adv_data();
	SEGGER_RTT_printf(0, "BLE: payload built (%d bytes)\n", adv_len);

	if (!api.ble.beacon.custom.payload.set(adv_payload, adv_len)) {
		SEGGER_RTT_printf(0, "BLE: ERROR - set custom payload failed\n");
		return -5;
	}

	SEGGER_RTT_printf(0, "BLE: starting advertise...\n");
	if (!api.ble.advertise.start(0)) {
		SEGGER_RTT_printf(0, "BLE: ERROR - advertise start failed\n");
		return -5;
	}

	SEGGER_RTT_printf(0, "BLE: advertising started (name=%s, %d bytes)\n",
		BLE_ADV_NAME, adv_len);
	return 0;
}

int ble_hub_adv_stop(void) {
	api.ble.advertise.stop();
	return 0;
}

void ble_hub_adv_update_data(void) {
	build_adv_data();
	api.ble.advertise.stop();
	api.ble.beacon.custom.payload.set(adv_payload, adv_len);
	api.ble.advertise.start(0);
}
