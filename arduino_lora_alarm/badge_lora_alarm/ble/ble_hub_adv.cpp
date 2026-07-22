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
#include "../debug_macros.h"

#define TAG "ble_hub_adv"

static uint8_t adv_payload[31];
static uint8_t adv_len = 0;

static void build_adv_data(void) {
	uint8_t *p = adv_payload;

	/* AD Flags */
	*p++ = 0x02; *p++ = 0x01; *p++ = 0x06;

	/* Complete Local Name: "ALARM_HUB" (9 chars) */
	const char *name = BLE_ADV_NAME;
	uint8_t name_len = strlen(name);
	*p++ = name_len + 1;
	*p++ = 0x09;  /* AD Type: Complete Local Name */
	memcpy(p, name, name_len); p += name_len;

	/* Manufacturer Data: 2B company ID + 6B MAC + 1B dev_type + 1B room_id */
	uint8_t mfg_len = 2 + 6 + 1 + 1;
	*p++ = mfg_len + 1;
	*p++ = 0xFF;  /* AD Type: Manufacturer Specific Data */
	*p++ = 0xAC;  /* Company ID low (0x04AC = RAKwireless) */
	*p++ = 0x04;

	/* MAC address (6 bytes, MSB) */
	uint8_t mac[8];
	api.system.chipId.get();  /* 无直接 MAC API, 用 chipId 前6字节近似 */
	/* 实际应从 SoftDevice 获取: api.ble.mac.get() 返回字符串, 需解析 */
	memset(mac, 0, 6);
	memcpy(p, mac, 6); p += 6;

	/* Device type + Room ID */
	*p++ = DEV_TYPE_HUB;
	*p++ = DEVICE_ROOM_ID;

	adv_len = p - adv_payload;
}

int ble_hub_adv_start(void) {
	api.ble.settings.blemode(RAK_BLE_BEACON_MODE);
	build_adv_data();

	if (!api.ble.beacon.custom.payload.set(adv_payload, adv_len)) {
		LOG_ERROR(TAG, "Set custom payload failed");
		return -5;
	}

	if (!api.ble.advertise.start(0)) {  /* 0 = 无限广播 */
		LOG_ERROR(TAG, "BLE advertise start failed");
		return -5;
	}

	LOG_INFO(TAG, "BLE advertising started (name=%s, interval=%dms)",
		BLE_ADV_NAME, BLE_ADV_INTERVAL_MS);
	return 0;
}

int ble_hub_adv_stop(void) {
	api.ble.advertise.stop();
	return 0;
}

void ble_hub_adv_update_data(void) {
	build_adv_data();
	/* 需要先停后启才能更新 payload */
	api.ble.advertise.stop();
	api.ble.beacon.custom.payload.set(adv_payload, adv_len);
	api.ble.advertise.start(0);
}
