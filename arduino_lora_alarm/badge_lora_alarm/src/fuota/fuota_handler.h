/*
 * FUOTA 固件升级 API
 */
#ifndef FUOTA_HANDLER_H
#define FUOTA_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化 FUOTA 回调 (覆盖 RUI3 默认的 LmhpFragmentationParams)
 *        必须在 app_hal_lorawan_init() 之后调用
 */
void fuota_init(void);

/**
 * @brief 检查是否有待处理的固件升级
 * @return true=有升级等待执行
 */
bool fuota_pending_check(void);

/**
 * @brief 执行待处理的固件升级 (CRC 校验 + Flash 搬运 + 重启)
 *        必须在系统启动早期调用 (BLE/LoRaWAN 启动前)
 *        成功时不会返回 (触发系统复位)
 * @return 0=无待处理升级, <0=校验失败, 永不返回成功
 */
int  fuota_apply_if_pending(void);

#endif /* FUOTA_HANDLER_H */
