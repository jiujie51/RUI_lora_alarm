/*
 * 配置持久化 — Flash + CRC32, RUI3 简化版
 * 仅存 group_id/device_type/hub_type/room_id (4 字节业务数据)
 * 偏移重映射至 RUI3 用户 Flash 区 (api.system.flash 基址 0x70000)
 */
#include <Arduino.h>
#include <string.h>
#include "config_store.h"
#include "../utils/crc32.h"
#include "../debug_macros.h"

#define TAG "config_store"

#define CONFIG_MAGIC     0x4C4F5241  /* "LORA" */
#define CONFIG_VERSION   1

/* RUI3 用户 Flash 相对偏移 (基址 0x70000) */
#define CONFIG_PRIMARY_OFFSET   0x00000
#define CONFIG_BACKUP1_OFFSET   0x01000
#define CONFIG_BACKUP2_OFFSET   0x02000
#define CONFIG_FACTORY_OFFSET   0x03000
#define CONFIG_SLOT_SIZE        64

static struct persistent_config current_config;
static const struct persistent_config factory_default = {
	.magic   = CONFIG_MAGIC,
	.version = CONFIG_VERSION,
	.group_id   = DEVICE_GROUP_ID,
	.device_type = DEVICE_TYPE,
	.hub_type   = DEVICE_HUB_TYPE,
	.room_id    = DEVICE_ROOM_ID,
};

static bool config_valid;

static uint32_t config_crc(const struct persistent_config *cfg) {
	return crc32_compute((const uint8_t *)cfg,
		offsetof(struct persistent_config, crc32));
}

static int read_config(uint32_t offset, struct persistent_config *cfg) {
	uint8_t raw[CONFIG_SLOT_SIZE];
	if (!api.system.flash.get(offset, raw, sizeof(raw))) return -5;

	memcpy(cfg, raw, sizeof(*cfg));
	if (cfg->magic != CONFIG_MAGIC) return -74;
	if (config_crc(cfg) != cfg->crc32) return -74;
	return 0;
}

static int write_config(uint32_t offset, const struct persistent_config *cfg) {
	struct persistent_config tmp;
	memcpy(&tmp, cfg, sizeof(tmp));
	tmp.magic   = CONFIG_MAGIC;
	tmp.version = CONFIG_VERSION;
	tmp.crc32   = config_crc(&tmp);

	return api.system.flash.set(offset, (uint8_t *)&tmp, sizeof(tmp)) ? 0 : -5;
}

int config_store_init(void) {
	int ret;

	ret = read_config(CONFIG_PRIMARY_OFFSET, &current_config);
	if (ret == 0) { config_valid = true; return 0; }

	ret = read_config(CONFIG_BACKUP1_OFFSET, &current_config);
	if (ret == 0) {
		config_valid = true;
		write_config(CONFIG_PRIMARY_OFFSET, &current_config);
		return 0;
	}

	ret = read_config(CONFIG_BACKUP2_OFFSET, &current_config);
	if (ret == 0) {
		config_valid = true;
		write_config(CONFIG_PRIMARY_OFFSET, &current_config);
		return 0;
	}

	/* Factory default */
	memcpy(&current_config, &factory_default, sizeof(current_config));
	current_config.crc32 = config_crc(&current_config);
	config_valid = true;
	config_store_save();

	LOG_INFO(TAG, "Config initialized from factory defaults (group=0x%02X room=%d)",
		current_config.group_id, current_config.room_id);
	return 0;
}

int config_store_save(void) {
	static int write_counter;

	current_config.magic   = CONFIG_MAGIC;
	current_config.version = CONFIG_VERSION;
	current_config.crc32   = config_crc(&current_config);

	if (!api.system.flash.set(CONFIG_PRIMARY_OFFSET,
		(uint8_t *)&current_config, sizeof(current_config)))
		return -5;

	uint32_t backup_off = (write_counter & 1) ?
		CONFIG_BACKUP1_OFFSET : CONFIG_BACKUP2_OFFSET;
	write_counter++;

	api.system.flash.set(backup_off,
		(uint8_t *)&current_config, sizeof(current_config));

	LOG_INFO(TAG, "Config saved (group=0x%02X)", current_config.group_id);
	return 0;
}

int config_store_factory_reset(void) {
	write_config(CONFIG_PRIMARY_OFFSET, &factory_default);
	memcpy(&current_config, &factory_default, sizeof(current_config));
	current_config.crc32 = config_crc(&current_config);
	config_valid = true;
	LOG_INFO(TAG, "Factory reset complete");
	return 0;
}

uint8_t config_get_group_id(void)  { return current_config.group_id; }
void    config_set_group_id(uint8_t id) { current_config.group_id = id; config_store_save(); }
uint8_t config_get_hub_type(void)  { return current_config.hub_type; }
void    config_set_hub_type(uint8_t t) { current_config.hub_type = t; config_store_save(); }
uint8_t config_get_room_id(void)   { return current_config.room_id; }
void    config_set_room_id(uint8_t id) { current_config.room_id = id; config_store_save(); }
bool    config_is_valid(void)      { return config_valid; }
