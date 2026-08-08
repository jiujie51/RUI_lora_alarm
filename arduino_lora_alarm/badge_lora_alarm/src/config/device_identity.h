/*
 * 设备身份 Flash 存储 — BLE MAC + LoRaWAN OTAA 凭证
 *
 * 存储地址: user flash offset 0x4000 (绝对 0xB4000, 独立 4KB 扇区)
 * 结构: magic(4) + ble_mac(6) + dev_eui(8) + app_eui(8) + app_key(16) + crc32(4) = 46 bytes
 *
 * 生产烧录流程:
 *   1. 工具脚本读取目标 BLE MAC (从 FICR 0x100000A4) + DevEUI/AppEUI/AppKey
 *   2. 打包为 device_identity 二进制, 计算 CRC32
 *   3. nrfjprog --memwr 0xB4000 --file identity.bin --sectorerase
 *
 * 首次上电检测: magic != 有效值 → 自动从 FICR 读取 BLE MAC, 凭证用编译期默认值
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>

/* 编译开关: 定义 IDENTITY_ENABLE 启用 flash 读写, 否则只用编译期默认值 */
#ifndef IDENTITY_ENABLE
// #define IDENTITY_ENABLE
#endif

/* ── 结构体定义 (46 bytes, packed) ── */
struct __attribute__((packed)) device_identity {
    uint32_t magic;         /* 0x4C534449 "IDSL" (Identity for LoRa System) */
    uint8_t  ble_mac[6];    /* BLE MAC (Badge: FICR IEEE public addr; Hub: SoftDevice random static) */
    uint8_t  dev_eui[8];    /* LoRaWAN DevEUI (LSBF) */
    uint8_t  app_eui[8];    /* LoRaWAN JoinEUI / AppEUI */
    uint8_t  app_key[16];   /* LoRaWAN AppKey */
    uint32_t crc32;         /* CRC32 of bytes 0..41 (magic through app_key) */
};

#define DEVICE_IDENTITY_MAGIC        0x4C534449  /* "IDSL" */
#define DEVICE_IDENTITY_FLASH_OFFSET 0x4000      /* user flash 偏移 */
#define DEVICE_IDENTITY_FLASH_ADDR   (0xB0000 + DEVICE_IDENTITY_FLASH_OFFSET) /* 绝对 0xB4000 */

/* ── API ── */

/**
 * @brief 初始化设备身份：从 flash 读取 (IDENTITY_ENABLE 定义时), 否则从 FICR 生成
 * @return 0=成功, <0=失败 (flash 读写错误)
 */
int  device_identity_init(void);

/**
 * @brief 检查设备身份是否有效 (magic + CRC32 校验通过)
 */
bool device_identity_is_valid(void);

/**
 * @brief 获取当前设备身份 (只读)
 */
const struct device_identity *device_identity_get(void);

/**
 * @brief 将设备身份写入 flash (预留, 供 AT 命令等外部接口调用)
 * @param id 新的设备身份数据 (CRC32 会被自动填入)
 * @return 0=成功, <0=失败
 */
int  device_identity_write(const struct device_identity *id);

/**
 * @brief 读取 nRF52840 FICR 中的出厂 BLE MAC 地址 (IEEE public address)
 * @param mac_out [out] 6 字节 MAC buffer
 */
void device_identity_read_ficr_mac(uint8_t mac_out[6]);

#endif /* DEVICE_IDENTITY_H */
