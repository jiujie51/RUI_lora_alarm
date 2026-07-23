/*
 * 电源管理 — RUI3 版 (电池 ADC 用 api.system.bat.get())
 * Hub 无充电检测, 始终返回 false
 */
#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdint.h>
#include <stdbool.h>

enum power_mode { POWER_NORMAL = 0, POWER_LOW_BATT };

int  power_mgr_init(void);
void power_mgr_update(void);

uint16_t power_mgr_get_voltage_mv(void);
uint8_t  power_mgr_get_battery_pct(void);
enum power_mode power_mgr_get_mode(void);
bool power_mgr_is_charging(void);
bool power_mgr_is_low_battery(void);

#endif
