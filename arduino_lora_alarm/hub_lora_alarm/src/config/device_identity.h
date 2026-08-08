/*
 * Device Identity Flash Storage — BLE MAC + LoRaWAN OTAA Credentials
 *
 * Flash address: user flash offset 0x4000 (absolute 0xB4000, independent 4KB sector)
 * Layout: magic(4) + ble_mac(6) + dev_eui(8) + app_eui(8) + app_key(16) + crc32(4) = 46 bytes
 *
 * Production:
 *   1. Read BLE MAC from api.ble.mac.get() (SoftDevice random static address)
 *      + DevEUI/AppEUI/AppKey
 *   2. Pack as device_identity binary with CRC32
 *   3. nrfjprog --program identity.hex --sectorerase --verify
 *
 * First boot: magic != valid → load defaults from api.ble.mac.get() + compile-time macros
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>

/* ── Struct (46 bytes, packed) ── */
struct __attribute__((packed)) device_identity {
	uint32_t magic;         /* 0x4C534449 "IDSL" (Identity for LoRa System) */
	uint8_t  ble_mac[6];    /* BLE MAC (api.ble.mac.get = SoftDevice random static addr) */
	uint8_t  dev_eui[8];    /* LoRaWAN DevEUI (LSBF) */
	uint8_t  app_eui[8];    /* LoRaWAN JoinEUI / AppEUI */
	uint8_t  app_key[16];   /* LoRaWAN AppKey */
	uint32_t crc32;         /* CRC32 of bytes 0..41 (magic through app_key) */
};

#define DEVICE_IDENTITY_MAGIC        0x4C534449  /* "IDSL" */
#define DEVICE_IDENTITY_FLASH_OFFSET 0x4000      /* RUI3 user flash offset */
#define DEVICE_IDENTITY_FLASH_ADDR   (0xB0000 + DEVICE_IDENTITY_FLASH_OFFSET) /* absolute 0xB4000 */

/* ── API ── */

/**
 * @brief Initialize device identity: read from flash, fallback to FICR + macros
 * @return 0=ok, <0=error (flash read failure)
 */
int  device_identity_init(void);

/**
 * @brief Check if device identity is valid (magic + CRC32 pass)
 */
bool device_identity_is_valid(void);

/**
 * @brief Get current device identity (read-only)
 */
const struct device_identity *device_identity_get(void);

/**
 * @brief Write device identity to flash (reserved, for AT command etc.)
 * @param id  new identity data (CRC32 will be computed automatically)
 * @return 0=ok, <0=error
 */
int  device_identity_write(const struct device_identity *id);

/**
 * @brief Persist current identity to flash using BLE MAC from SoftDevice.
 *        Only writes if identity was NOT loaded from flash (first-boot scenario).
 *        BLE must be initialized before calling (api.ble.mac.get() needs SoftDevice).
 * @return 0=ok or already persisted, <0=error
 */
int  device_identity_persist(void);

/**
 * @brief Check whether identity was loaded from flash (vs RAM fallback)
 * @return true if identity came from valid flash data
 */
bool device_identity_is_from_flash(void);

/**
 * @brief Read BLE MAC from SoftDevice (api.ble.mac.get) — random static address
 * @param mac_out [out] 6-byte MAC buffer
 */
void device_identity_read_ble_mac(uint8_t mac_out[6]);

/**
 * @brief Read factory BLE MAC from nRF52840 FICR (IEEE public address, for reference)
 * @param mac_out [out] 6-byte MAC buffer
 */
void device_identity_read_ficr_mac(uint8_t mac_out[6]);

#endif /* DEVICE_IDENTITY_H */
