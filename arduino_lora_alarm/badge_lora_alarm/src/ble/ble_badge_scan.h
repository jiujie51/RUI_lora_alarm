/*
 * BLE Badge 扫描 — Hub 广播发现 + RSSI 定位 (Badge 专属)
 *
 * Hub 广播格式 (ble_hub_adv.cpp):
 *   [Complete Name: "ALARM_HUB"]
 *   [Manufacturer Data: MAC(6) + dev_type(1) + room_id(1)]
 *
 * 算法: 收集多个 Hub 的 RSSI 样本, 取中值排序, 选信号最强的上报.
 *       告警扫描: 4s 连续扫描. 静默扫描: 每 30s 扫描 1s.
 *
 * 参考: ncs_lora_alarm/ble/ble_badge_scan.c
 *       RUI3 Example/BLE_Scanner/BLE_Scanner.ino
 */
#ifndef BLE_BADGE_SCAN_H
#define BLE_BADGE_SCAN_H

#include <stdint.h>
#include <stdbool.h>

/* ── 扫描结果 ── */
struct ble_scan_result {
	uint8_t  hub_mac[6];    /* 最强 Hub 的 MAC */
	int8_t   rssi;          /* RSSI (dBm) */
	uint8_t  room_id;       /* Hub 广播的房间 ID */
	uint32_t timestamp;     /* 扫描完成时间 (millis) */
	bool     valid;         /* 结果有效 */
};

/* ── 时序 ── */
#define SCAN_ALERT_DURATION_MS   4000   /* 告警扫描 4s */
#define SCAN_SILENT_INTERVAL_MS  30000  /* 静默扫描间隔 30s */
#define SCAN_SILENT_DURATION_MS  1000   /* 静默扫描窗 1s */

/* ── API ── */
int  ble_scan_init(void);                       /* 初始化, 注册回调 */
int  ble_scan_reinit(void);                      /* 轻量重初始化: 只清数据和回调, 不启停 BLE (避免 SD assert) */
int  ble_scan_start_alert(void);                 /* 开始 4s 告警扫描 */
int  ble_scan_start_silent(void);                /* 开始 1s 静默扫描 */
int  ble_scan_stop(void);                        /* 停止扫描 + 选最佳 Hub */
const struct ble_scan_result *ble_scan_get_result(void); /* 获取结果 */
void ble_scan_process(void);                     /* 周期性处理 (10ms) */
bool ble_scan_active(void);                      /* 扫描中? */

#endif
