/* CRC32 (IEEE 802.3) 工具 — 直接复制自 NCS, 零依赖 */
#ifndef UTILS_CRC32_H
#define UTILS_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t crc32_compute(const uint8_t *data, size_t len);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

#endif
