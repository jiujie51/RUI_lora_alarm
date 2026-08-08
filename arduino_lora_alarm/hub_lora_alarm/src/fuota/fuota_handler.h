/*
 * FUOTA Firmware Upgrade — RUI3 missing parts: Flash firmware swap + Boot check
 *
 * RUI3 already provides (when SUPPORT_FUOTA is enabled):
 *   - FragDecoder fragment reception/FEC decoding/write to FW_LOCATION
 *   - OnFragProgress progress log (service_lora_fuota.c)
 *   - OnFragDone completion notification (service_lora_fuota.c, demo stub only)
 *   - LmhpFragmentationParams callback registration
 *
 * This module supplements what RUI3 lacks:
 *   1. fuota_init() — runtime override LmhpFragmentationParams.OnDone,
 *      replacing RUI3's demo OnFragDone with CRC verify + update flag write
 *   2. fuota_apply_if_pending() — boot-time check of update flag, firmware swap from RAM
 *   3. fw_swap_from_ram() — inline NVMC erase+copy (must not call flash-resident library)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize FUOTA callbacks (overrides RUI3 default LmhpFragmentationParams)
 *        Must be called after app_hal_lorawan_init()
 */
void fuota_init(void);

/**
 * @brief Check for pending firmware update
 * @return true=update pending
 */
bool fuota_pending_check(void);

/**
 * @brief Apply pending firmware update (CRC verify + Flash swap + Reset)
 *        Must be called early in boot (before BLE/LoRaWAN init)
 *        Does not return on success (triggers system reset)
 * @return 0=no pending update, <0=verify failed, never returns success
 */
int  fuota_apply_if_pending(void);
