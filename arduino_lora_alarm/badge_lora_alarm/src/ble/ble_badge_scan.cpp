#include <stdint.h>
/*
 * BLE Badge 扫描 — Hub 发现 + RSSI 定位
 * 移植自 NCS ble_badge_scan.c, 适配 RUI3 api.ble.scanner
 *
 * Hub 广播 AD 结构:
 *   [0x0A, 0x09, "ALARM_HUB"]              ← Complete Local Name
 *   [0x09, 0xFF, MAC(6)+dev_type(1)+room_id(1)] ← Manufacturer Data
 */
#include <Arduino.h>
#include <string.h>
#include "ble_badge_scan.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── AD 类型 ── */
#define BLE_AD_TYPE_NAME_SHORT      0x08
#define BLE_AD_TYPE_NAME_COMPLETE   0x09
#define BLE_AD_TYPE_MANUFACTURER    0xFF

#define HUB_NAME  "ALARM_HUB"
#define MAX_HUBS  8
#define RSSI_SAMPLES 4

/* ── 单个 Hub 跟踪 ── */
struct hub_info {
	uint8_t  mac[6];
	int8_t   rssi_samples[RSSI_SAMPLES];
	uint8_t  sample_idx;
	uint8_t  room_id;
	bool     active;
};

static struct hub_info hubs[MAX_HUBS];
static struct ble_scan_result last_result;
static bool    scanning;
static bool    alert_mode;
static uint32_t scan_start_ms;
static uint32_t scan_duration_ms;
static uint32_t last_silent_ms;

/* ── 解析 AD 结构, 找到名称和 room_id ── */
static bool parse_adv_data(const uint8_t *data, uint16_t len,
			   char *name_out, uint8_t name_max,
			   uint8_t *room_id_out) {
	bool have_name = false;
	uint16_t pos = 0;

	while (pos + 1 < len) {
		uint8_t field_len = data[pos];
		if (field_len == 0 || pos + 1 + field_len > len) break;
		uint8_t field_type = data[pos + 1];
		const uint8_t *field_data = &data[pos + 2];
		uint8_t data_len = field_len - 1;

		if (field_type == BLE_AD_TYPE_NAME_COMPLETE ||
		    field_type == BLE_AD_TYPE_NAME_SHORT) {
			uint8_t n = (data_len < name_max - 1) ? data_len : name_max - 1;
			memcpy(name_out, field_data, n);
			name_out[n] = '\0';
			have_name = true;
		} else if (field_type == BLE_AD_TYPE_MANUFACTURER) {
			/* RUI3 Hub: CompanyID(2) + MAC(6) + dev_type(1) + room_id(1) = 10 bytes */
			if (data_len >= 10) {
				*room_id_out = field_data[9];
			}
			/* NCS Hub (无 CompanyID): MAC(6) + dev_type(1) + room_id(1) = 8 bytes */
			else if (data_len >= 8) {
				*room_id_out = field_data[7];
			}
		}
		pos += field_len + 1;
	}
	return have_name;
}

/* ── 查找或创建 Hub 条目 ── */
static struct hub_info *find_or_create_hub(const uint8_t *mac, uint8_t room_id) {
	/* 查找已有条目 */
	for (int i = 0; i < MAX_HUBS; i++) {
		if (hubs[i].active && memcmp(hubs[i].mac, mac, 6) == 0) {
			hubs[i].room_id = room_id;
			return &hubs[i];
		}
	}
	/* 创建新条目 */
	for (int i = 0; i < MAX_HUBS; i++) {
		if (!hubs[i].active) {
			memset(&hubs[i], 0, sizeof(hubs[i]));
			memcpy(hubs[i].mac, mac, 6);
			hubs[i].active = true;
			hubs[i].room_id = room_id;
			return &hubs[i];
		}
	}
	return NULL; /* 满了 */
}

/* ── RUI3 扫描回调 (ISR 上下文, 栈极紧, 局部大数组用 static) ── */
static void on_scan_result(int8_t rssi, uint8_t *device_mac,
			   uint8_t *scan_data, uint16_t data_len) {
	if (!scanning) return;
	if (!device_mac || !scan_data || data_len == 0) return;

	static char name[32];
	uint8_t room_id = 0;

	bool have_name = parse_adv_data(scan_data, data_len, name, sizeof(name), &room_id);
	if (!have_name || strncmp(name, HUB_NAME, strlen(HUB_NAME)) != 0) return;

	struct hub_info *hub = find_or_create_hub(device_mac, room_id);
	if (!hub) return;

	hub->rssi_samples[hub->sample_idx % RSSI_SAMPLES] = rssi;
	hub->sample_idx++;
}

/* ── 选中值排序, 选最强 Hub ── */
static void select_best_hub(void) {
	struct hub_info *best = NULL;
	int8_t best_rssi = -128;

	for (int i = 0; i < MAX_HUBS; i++) {
		if (!hubs[i].active || hubs[i].sample_idx == 0) continue;

		/* 取中值 (排序后取中间) */
		uint8_t n = (hubs[i].sample_idx < RSSI_SAMPLES) ? hubs[i].sample_idx : RSSI_SAMPLES;
		int8_t sorted[RSSI_SAMPLES];
		memcpy(sorted, hubs[i].rssi_samples, n);
		for (int a = 0; a < n - 1; a++)
			for (int b = a + 1; b < n; b++)
				if (sorted[a] < sorted[b]) {
					int8_t t = sorted[a]; sorted[a] = sorted[b]; sorted[b] = t;
				}
		int8_t median = sorted[n / 2];

		/* 已有最佳 Hub 时, 新 Hub 需显著更强 (6dB 滞后) */
		if (best && (best_rssi - median) > 6) continue;
		/* 首次, 忽略极弱信号 (< -95dBm) */
		if (!best && median < -95) continue;
		if (median > best_rssi) {
			best_rssi = median;
			best = &hubs[i];
		}
	}

	if (best) {
		memcpy(last_result.hub_mac, best->mac, 6);
		last_result.rssi     = best_rssi;
		last_result.room_id  = best->room_id;
		last_result.valid    = true;
		last_result.timestamp = millis();
		SEGGER_RTT_printf(0, "[BLE] best hub: MAC=%02X:%02X:%02X:%02X:%02X:%02X RSSI=%d room=%d\n",
			best->mac[5], best->mac[4], best->mac[3],
			best->mac[2], best->mac[1], best->mac[0],
			best_rssi, best->room_id);
	} else {
		last_result.valid = false;
		SEGGER_RTT_printf(0, "[INFO] No hub found in scan\n");
	}
}

/* ── 公共 API ── */
int ble_scan_init(void) {
	memset(hubs, 0, sizeof(hubs));
	memset(&last_result, 0, sizeof(last_result));
	scanning    = false;
	alert_mode  = false;
	last_silent_ms = 0;

	api.ble.scanner.setScannerCallback(on_scan_result);
	/* 触发 BLE 协议栈初始化 (必须在 LoRaWAN join 之前, 否则冲突重启) */
	api.ble.scanner.start(0);
	delay(100);
	api.ble.scanner.stop();
	SEGGER_RTT_printf(0, "[INFO] BLE scanner initialized\n");
	return 0;
}

/* 轻量重初始化 — 仅清数据和回调, 不启停 BLE
 * 避免 LoRaWAN join 后重复调用 start/stop 触发 SoftDevice 断言 (NRF_FAULT_ID_SD_ASSERT)
 */
int ble_scan_reinit(void) {
	memset(hubs, 0, sizeof(hubs));
	memset(&last_result, 0, sizeof(last_result));
	scanning    = false;
	alert_mode  = false;
	/* 重新注册回调 (SoftDevice 可能在 LoRaWAN join 时清除了它) */
	api.ble.scanner.setScannerCallback(on_scan_result);
	SEGGER_RTT_printf(0, "[INFO] BLE scanner re-initialized (lightweight)\n");
	return 0;
}

static void start_scan(uint32_t duration_ms) {
	if (scanning) ble_scan_stop();

	memset(hubs, 0, sizeof(hubs));
	memset(&last_result, 0, sizeof(last_result));

	api.ble.scanner.setInterval(1000, 500);
	api.ble.scanner.start(0);

	scanning        = true;
	scan_start_ms   = millis();
	scan_duration_ms = duration_ms;
}

int ble_scan_start_alert(void) {
	alert_mode = true;
	if (scanning) {
		/* 已在扫描中 (alert/silent), 仅延长超时, 避免 SoftDevice stop→start 竞态 */
		scan_start_ms    = millis();
		scan_duration_ms = SCAN_ALERT_DURATION_MS;
		SEGGER_RTT_printf(0, "[INFO] Alert scan extended (%ds)\n", SCAN_ALERT_DURATION_MS / 1000);
		return 0;
	}
	start_scan(SCAN_ALERT_DURATION_MS);
	SEGGER_RTT_printf(0, "[INFO] Alert scan started (%ds)\n", SCAN_ALERT_DURATION_MS / 1000);
	return 0;
}

int ble_scan_start_silent(void) {
	uint32_t now = millis();
	if (now - last_silent_ms < SCAN_SILENT_INTERVAL_MS) return 0;

	alert_mode = false;
	last_silent_ms = now;
	start_scan(SCAN_SILENT_DURATION_MS);
	return 0;
}

int ble_scan_stop(void) {
	if (!scanning) return 0;
	SEGGER_RTT_printf(0, "[INFO] ble scan stop\n");
	api.ble.scanner.stop();
	scanning = false;
	select_best_hub();
	return 0;
}

const struct ble_scan_result *ble_scan_get_result(void) {
	return &last_result;
}

bool ble_scan_active(void) {
	return scanning;
}

void ble_scan_process(void) {
	if (!scanning) return;
	if ((millis() - scan_start_ms) >= scan_duration_ms) {
		ble_scan_stop();
	}
}
