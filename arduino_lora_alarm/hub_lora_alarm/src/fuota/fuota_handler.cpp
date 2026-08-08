/*
 * FUOTA 固件升级 — RUI3 缺失部分: Flash 固件搬运 + 启动升级检查
 *
 * RUI3 已有的 (SUPPORT_FUOTA 启用后自动生效):
 *   - FragDecoder 分片接收/FEC 解码/写入 FW_LOCATION
 *   - OnFragProgress 进度日志 (service_lora_fuota.c)
 *   - OnFragDone 完成通知 (service_lora_fuota.c, 但仅 demo 读 flash)
 *   - LmhpFragmentationParams 回调注册
 *
 * 本模块补充 RUI3 缺失的:
 *   1. fuota_init() — 运行时覆盖 LmhpFragmentationParams.OnDone,
 *      替换 RUI3 的 demo OnFragDone 为带 CRC 校验 + 写升级标记的版本
 *   2. fuota_apply_if_pending() — 启动时检查升级标记, 从 RAM 搬运固件
 *   3. fw_swap_from_ram() — 内联 NVMC 擦除+复制 (不能调 flash 中的库函数)
 */

#include <stdint.h>
#include <string.h>
#include <Arduino.h>

extern "C" {
#include "nrf_nvmc.h"
#include "nrf.h"
int SEGGER_RTT_printf(unsigned, const char*, ...);
}

#define APP_FLASH_BASE        0x27000
#define FW_UPDATE_FLASH_ADDR  0x000BA000   /* 升级标记页 (user data 区) */
#define FW_UPDATE_MAGIC       0x46555550   /* "FUUP" */

#ifndef FW_LOCATION
#define FW_LOCATION           0x7D000
#endif

struct fw_update_flag {
    uint32_t magic;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t reserved;
};

/* ── CRC32 (CCITT, 与 FragDecoder 一致) ── */
static uint32_t fuota_crc32(const uint8_t *buf, uint32_t len)
{
    const uint32_t poly = 0xEDB88320;
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (poly & ~((crc & 0x01) - 1));
    }
    return ~crc;
}

/* ── 内联 NVMC (不能调 nrf_nvmc_* 库函数: 固件搬运时它们所在的 flash 页会被擦除) ── */
__STATIC_INLINE void ram_nvmc_enable(void)  { NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een; __DSB(); __ISB(); }
__STATIC_INLINE void ram_nvmc_disable(void) { NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren; __DSB(); __ISB(); }
__STATIC_INLINE void ram_nvmc_erase_page(uint32_t addr) {
    ram_nvmc_enable(); NRF_NVMC->ERASEPAGE = addr;
    while (!NRF_NVMC->READY) { __NOP(); } ram_nvmc_disable();
}
__STATIC_INLINE void ram_nvmc_write(uint32_t addr, uint32_t val) {
    ram_nvmc_enable(); *(volatile uint32_t *)addr = val;
    while (!NRF_NVMC->READY) { __NOP(); } ram_nvmc_disable();
}

/* ── 固件搬运: 全部内联, .ramfunc 确保在 RAM 中执行 ── */
__attribute__((section(".ramfunc"), noinline, used, optimize("Os")))
static void fw_swap_from_ram(uint32_t fw_size)
{
    uint32_t page_sz = NRF_FICR->CODEPAGESIZE;
    uint32_t pages   = (FW_LOCATION - APP_FLASH_BASE + page_sz - 1) / page_sz;

    for (uint32_t p = 0; p < pages; p++)
        ram_nvmc_erase_page(APP_FLASH_BASE + p * page_sz);

    uint32_t words = (fw_size + 3) / 4;
    volatile uint32_t *src = (volatile uint32_t *)FW_LOCATION;
    for (uint32_t i = 0; i < words; i++)
        ram_nvmc_write(APP_FLASH_BASE + i * 4, src[i]);

    ram_nvmc_erase_page(FW_UPDATE_FLASH_ADDR);
    NVIC_SystemReset();
    for (;;) { __NOP(); }
}

/* ═══════════════════════════════════════════════
 * 公共 API
 * ═══════════════════════════════════════════════ */

/* 启动时调用: 检查并执行待处理的固件升级 (成功时不返回) */
int fuota_apply_if_pending(void)
{
    struct fw_update_flag flag;
    memcpy(&flag, (const void *)FW_UPDATE_FLASH_ADDR, sizeof(flag));

    if (flag.magic != FW_UPDATE_MAGIC)
        return 0;  /* 无待处理升级 */

    SEGGER_RTT_printf(0, "[FUOTA] === Pending update: size=%lu crc=0x%08lX ===\r\n",
        (unsigned long)flag.fw_size, flag.fw_crc32);

    /* CRC 校验 */
    uint32_t crc = fuota_crc32((const uint8_t *)FW_LOCATION, flag.fw_size);
    if (crc != flag.fw_crc32) {
        SEGGER_RTT_printf(0, "[FUOTA] CRC FAIL — discarding\r\n");
        nrf_nvmc_page_erase(FW_UPDATE_FLASH_ADDR);
        return -1;
    }

    SEGGER_RTT_printf(0, "[FUOTA] CRC OK — swapping firmware...\r\n");
    delay(200);
    fw_swap_from_ram(flag.fw_size);
    return 0;  /* unreachable */
}

#ifdef SUPPORT_FUOTA
#include "LmhpFragmentation.h"

/*
 * 替换 RUI3 默认 OnFragDone:
 * RUI3 service_lora_fuota.c 中的 OnFragDone 只是 demo 桩 (读 0x7D000 打日志).
 * 我们在运行时覆盖 LmhpFragmentationParams.OnDone, 指向这里的实现:
 *   → CRC32 校验 FW_LOCATION 中的固件
 *   → 写升级标记 (magic + size + crc) 到 FW_UPDATE_FLASH_ADDR
 *   → NVIC_SystemReset
 */
static void fuota_on_frag_done(int32_t status, uint32_t size)
{
    SEGGER_RTT_printf(0, "[FUOTA] OnFragDone: status=%ld size=%lu\r\n",
        (long)status, (unsigned long)size);

    if (status < 0 || size == 0 || size > (0x100000 - FW_LOCATION)) {
        SEGGER_RTT_printf(0, "[FUOTA] Invalid — discarded\r\n");
        return;
    }

    uint32_t crc = fuota_crc32((const uint8_t *)FW_LOCATION, size);
    SEGGER_RTT_printf(0, "[FUOTA] CRC32: 0x%08lX\r\n", crc);

    struct fw_update_flag flag = { FW_UPDATE_MAGIC, size, crc, 0 };

    nrf_nvmc_page_erase(FW_UPDATE_FLASH_ADDR);
    volatile uint32_t *dst = (volatile uint32_t *)FW_UPDATE_FLASH_ADDR;
    uint32_t *src = (uint32_t *)&flag;
    for (uint32_t i = 0; i < (sizeof(flag) + 3) / 4; i++)
        nrf_nvmc_write_word((uint32_t)(dst + i), src[i]);

    SEGGER_RTT_printf(0, "[FUOTA] Update flag saved — rebooting\r\n");
    delay(500);
    NVIC_SystemReset();
}

void fuota_init(void)
{
    /* RUI3 已在 service_lora.c 初始化时注册 LmhpFragmentationParams.
     * 此处运行时替换 OnDone 回调 (OnProgress 保留 RUI3 原版, 已打日志). */
    extern LmhpFragmentationParams_t LmhpFragmentationParams;
    LmhpFragmentationParams.OnDone = fuota_on_frag_done;

    SEGGER_RTT_printf(0, "[FUOTA] OnDone hooked (FW_LOCATION=0x%08lX)\r\n",
        (unsigned long)FW_LOCATION);
}
#else
void fuota_init(void)
{
    SEGGER_RTT_printf(0, "[FUOTA] SUPPORT_FUOTA not enabled\r\n");
}
#endif
