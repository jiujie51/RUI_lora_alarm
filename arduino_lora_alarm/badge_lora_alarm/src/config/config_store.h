/* 配置持久化 — 包含应用层参数 + CMD 0x04 告警配置
 * LoRaWAN 凭证/DevNonce/会话密钥由 RUI3 service_nvm 自动管理
 */
#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "../app/actuator_mgr.h"

struct persistent_config {
	uint32_t magic;
	uint32_t version;
	uint32_t crc32;
	uint8_t  group_id;
	uint8_t  device_type;
	uint8_t  hub_type;
	uint8_t  room_id;
	struct   alarm_config alarm_cfg;  /* CMD 0x04 告警配置 (LED/buzzer/vib) */
};

int  config_store_init(void);
int  config_store_save(void);
int  config_store_factory_reset(void);

uint8_t config_get_group_id(void);
void    config_set_group_id(uint8_t id);
uint8_t config_get_hub_type(void);
void    config_set_hub_type(uint8_t type);
uint8_t config_get_room_id(void);
void    config_set_room_id(uint8_t id);

/* 告警配置持久化 */
const struct alarm_config *config_get_alarm_config(void);
void config_save_alarm_config(const struct alarm_config *cfg);

bool config_is_valid(void);

#endif
