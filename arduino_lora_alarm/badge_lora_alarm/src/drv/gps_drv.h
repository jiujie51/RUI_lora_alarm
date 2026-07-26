/*
 * RAK12501 GPS 驱动 — TinyGPSPlus + Serial1 (RUI3/Arduino)
 *
 * 硬件: RAK12501 (u-blox MAX-7Q) via WisBlock IO slot
 *       Serial1 RX=P0.15, TX=P0.16, 9600bps
 *       上电控制: GPS_PWR_PIN, 复位: GPS_RESET_PIN
 * NMEA 解析: TinyGPSPlus 库 (http://librarymanager/All#TinyGPSPlus)
 *
 * 参考: examples/RAK12501/RAK12501_GPS_L76K.ino
 */
#ifndef GPS_DRV_H
#define GPS_DRV_H

#include <stdint.h>
#include <stdbool.h>

/* ── API ── */

/* 初始化 GPS (上电 + Serial1.begin) */
int  gps_drv_init(void);

/* 轮询 Serial1 并喂 TinyGPSPlus 解析 (每 ~100ms 调用) */
void gps_drv_poll(void);

/* 是否有有效定位 */
bool gps_drv_has_fix(void);

/* 获取位置 (度 × 1e6) — 兼容 NCS proto_build_key_event */
int  gps_drv_get_position(int32_t *lat, int32_t *lon);

/* 获取卫星数 */
uint8_t gps_drv_satellites(void);

/* GPS 数据是否过期 (>30s 无更新) */
bool gps_drv_is_stale(void);

/* 超时检查, 自动失效 */
void gps_drv_check_timeout(void);

#endif /* GPS_DRV_H */
