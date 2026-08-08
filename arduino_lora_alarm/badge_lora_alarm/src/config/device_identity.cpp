#include <stdint.h>
/*
 * 设备身份 Flash 存储 — 实现
 *
 * 读取 BLE MAC (nRF52840 FICR) → 合并 DevEUI/AppEUI/AppKey → 写入单扇区
 * 存储地址: user flash offset 0x4000 (绝对 0xB4000)
 */

#include <Arduino.h>
#include <string.h>
#include "device_identity.h"
#include "../boards/badge/board.h"
#include "../utils/crc32.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

static struct device_identity g_identity;
static bool g_identity_valid = false;
static bool g_identity_from_flash = false;

/* ── 从 nRF52840 FICR 读取出厂 BLE MAC ── */
void device_identity_read_ficr_mac(uint8_t mac_out[6])
{
    uint32_t addr0 = NRF_FICR->DEVICEADDR[0];
    uint32_t addr1 = NRF_FICR->DEVICEADDR[1];

    /* nRF52840 FICR DEVICEADDR[0..1] 存储 public BLE address (LSBF) */
    mac_out[0] = (addr0 >> 0)  & 0xFF;
    mac_out[1] = (addr0 >> 8)  & 0xFF;
    mac_out[2] = (addr0 >> 16) & 0xFF;
    mac_out[3] = (addr0 >> 24) & 0xFF;
    mac_out[4] = (addr1 >> 0)  & 0xFF;
    mac_out[5] = (addr1 >> 8)  & 0xFF;

    SEGGER_RTT_printf(0, "[IDENT] FICR BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
        mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5]);
}

/* ── CRC32 计算 (不含 crc32 字段自身) ── */
static uint32_t identity_crc(const struct device_identity *id)
{
    return crc32_compute((const uint8_t *)id,
        offsetof(struct device_identity, crc32));
}

/* ── 从 flash 读取 ── */
static int identity_read_flash(struct device_identity *id)
{
    uint8_t raw[sizeof(*id) + 16]; /* 多读一些, 避免边界问题 */
    memset(raw, 0, sizeof(raw));

    if (!api.system.flash.get(DEVICE_IDENTITY_FLASH_OFFSET, raw, sizeof(*id))) {
        SEGGER_RTT_printf(0, "[IDENT] Flash read FAIL at offset 0x%04lX\r\n",
            (unsigned long)DEVICE_IDENTITY_FLASH_OFFSET);
        return -1;
    }

    /* Debug: dump raw bytes */
    SEGGER_RTT_printf(0, "[IDENT] Raw flash bytes (%u):\r\n", (unsigned)sizeof(*id));
    for (unsigned i = 0; i < sizeof(*id); i++) {
        SEGGER_RTT_printf(0, "%02X ", raw[i]);
        if ((i + 1) % 16 == 0) SEGGER_RTT_printf(0, "\r\n");
    }
    SEGGER_RTT_printf(0, "\r\n");
    SEGGER_RTT_printf(0, "[IDENT] sizeof(id)=%u offsetof(crc32)=%u\r\n",
        (unsigned)sizeof(*id), (unsigned)offsetof(struct device_identity, crc32));

    memcpy(id, raw, sizeof(*id));
    return 0;
}

/* ── 校验 ── */
static bool identity_validate(const struct device_identity *id)
{
    if (id->magic != DEVICE_IDENTITY_MAGIC) {
        SEGGER_RTT_printf(0, "[IDENT] Bad magic: 0x%08lX (expected 0x%08lX)\r\n",
            (unsigned long)id->magic, (unsigned long)DEVICE_IDENTITY_MAGIC);
        return false;
    }
    uint32_t computed = identity_crc(id);
    if (computed != id->crc32) {
        SEGGER_RTT_printf(0, "[IDENT] CRC mismatch: calc=0x%08lX stored=0x%08lX\r\n",
            (unsigned long)computed, (unsigned long)id->crc32);
        return false;
    }
    return true;
}

/* ── 用编译期宏生成默认身份 (fallback) ── */
static void identity_make_default(struct device_identity *id)
{
    memset(id, 0, sizeof(*id));
    id->magic = DEVICE_IDENTITY_MAGIC;

    /* BLE MAC from FICR */
    device_identity_read_ficr_mac(id->ble_mac);

    /* LoRaWAN 凭证 from board.h */
    {
        uint8_t tmp_eui[8]  = OTAA_DEVEUI;
        uint8_t tmp_join[8] = OTAA_APPEUI;
        uint8_t tmp_key[16] = OTAA_APPKEY;
        memcpy(id->dev_eui, tmp_eui, 8);
        memcpy(id->app_eui, tmp_join, 8);
        memcpy(id->app_key, tmp_key, 16);
    }

    id->crc32 = identity_crc(id);
}

/* ── 打印身份信息 ── */
static void identity_log(const struct device_identity *id)
{
    SEGGER_RTT_printf(0, "[IDENT] MAC:   %02X:%02X:%02X:%02X:%02X:%02X\r\n",
        id->ble_mac[0], id->ble_mac[1], id->ble_mac[2],
        id->ble_mac[3], id->ble_mac[4], id->ble_mac[5]);

    SEGGER_RTT_printf(0, "[IDENT] DevEUI: ");
    for (int i = 0; i < 8; i++)
        SEGGER_RTT_printf(0, "%02X", id->dev_eui[i]);
    SEGGER_RTT_printf(0, "\r\n");

    SEGGER_RTT_printf(0, "[IDENT] AppEUI: ");
    for (int i = 0; i < 8; i++)
        SEGGER_RTT_printf(0, "%02X", id->app_eui[i]);
    SEGGER_RTT_printf(0, "\r\n");

    SEGGER_RTT_printf(0, "[IDENT] AppKey: ");
    for (int i = 0; i < 16; i++)
        SEGGER_RTT_printf(0, "%02X", id->app_key[i]);
    SEGGER_RTT_printf(0, "\r\n");

    SEGGER_RTT_printf(0, "[IDENT] CRC:   0x%08lX\r\n", (unsigned long)id->crc32);
}

/* ══════════════════════════════════════════════════
 * 公共 API
 * ══════════════════════════════════════════════════ */

int device_identity_init(void)
{
    struct device_identity id;

    /* 1. 尝试从 flash 读取 */
    #if IDENTITY_ENABLE
    if (identity_read_flash(&id) == 0 && identity_validate(&id)) {
        memcpy(&g_identity, &id, sizeof(g_identity));
        g_identity_valid = true;
        g_identity_from_flash = true;
        SEGGER_RTT_printf(0, "[IDENT] Loaded from flash (addr 0x%04lX)\r\n",
            (unsigned long)DEVICE_IDENTITY_FLASH_OFFSET);
        identity_log(&g_identity);
        return 0;
    }
    #endif

    /* 2. Flash 无效 → 从编译期宏 + FICR 生成默认值 (仅 RAM, 不写 flash) */
    SEGGER_RTT_printf(0, "[IDENT] Flash invalid or empty, using defaults (RAM only)\r\n");
    identity_make_default(&id);

    memcpy(&g_identity, &id, sizeof(g_identity));
    g_identity_valid = true;
    g_identity_from_flash = false;
    identity_log(&g_identity);
    return 0;
}

/* ── 写入 flash (预留, 生产时通过 AT 命令或外部工具调用) ── */
static int identity_write_flash(const struct device_identity *id)
{
    if (!api.system.flash.set(DEVICE_IDENTITY_FLASH_OFFSET,
        (uint8_t *)id, sizeof(*id))) {
        SEGGER_RTT_printf(0, "[IDENT] Flash write FAIL at offset 0x%04lX\r\n",
            (unsigned long)DEVICE_IDENTITY_FLASH_OFFSET);
        return -2;
    }
    return 0;
}

int device_identity_write(const struct device_identity *id)
{
    struct device_identity tmp;
    memcpy(&tmp, id, sizeof(tmp));
    tmp.magic = DEVICE_IDENTITY_MAGIC;
    tmp.crc32 = identity_crc(&tmp);

    if (identity_write_flash(&tmp) != 0)
        return -2;

    memcpy(&g_identity, &tmp, sizeof(g_identity));
    g_identity_valid = true;
    g_identity_from_flash = true;
    SEGGER_RTT_printf(0, "[IDENT] Updated + persisted to flash\r\n");
    identity_log(&g_identity);
    return 0;
}

bool device_identity_is_valid(void)
{
    return g_identity_valid;
}

const struct device_identity *device_identity_get(void)
{
    return &g_identity;
}

