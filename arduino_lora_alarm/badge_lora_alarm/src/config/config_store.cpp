#include <stdint.h>
/*
 * 配置持久化 — Flash + CRC32, RUI3 简化版
 * 存储: group_id/device_type/hub_type/room_id + alarm_config (CMD 0x04)
 * 偏移重映射至 RUI3 用户 Flash 区 (api.system.flash 基址 0xB0000 = MCU_USER_DATA_NVM_ADDR)
 */
#include <Arduino.h>
#include <string.h>
#include "config_store.h"
#include "../boards/badge/board.h"
#include "../utils/crc32.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

#define CONFIG_MAGIC     0x4C4F5241  /* "LORA" */
#define CONFIG_VERSION   2           /* V2: 增加 alarm_config */

/* RUI3 用户 Flash 相对偏移 (基址 0xB0000 = MCU_USER_DATA_NVM_ADDR) */
#define CONFIG_PRIMARY_OFFSET   0x00000
#define CONFIG_BACKUP1_OFFSET   0x01000
#define CONFIG_BACKUP2_OFFSET   0x02000
#define CONFIG_FACTORY_OFFSET   0x03000
#define CONFIG_SLOT_SIZE        256       /* struct persistent_config ~150B, 余量充足 */

static struct persistent_config current_config;

/* 默认 alarm_config (与 actuator_mgr default_config 保持一致) */
static const struct alarm_config default_alarm_cfg = {
	.led_map = {
		[0] = {255, 0,   0,   4, 300,  300},
		[1] = {180, 0,   255, 2, 500,  500},
		[2] = {255, 120, 0,   2, 500,  500},
		[3] = {180, 0,   255, 1, 0,    0},
		[4] = {180, 0,   255, 2, 1000, 1000},
		[5] = {0,   80,  255, 2, 500,  500},
		[6] = {255, 220, 0,   2, 500,  500},
		[7] = {0,   255, 60,  1, 0,    0},
		[8] = {0,   0,   0,   0, 0,    0},
	},
	.buzzer_map = {
		[0] = {2, 5,  120, 80 },
		[1] = {2, 5,  500, 500},
		[2] = {2, 5,  300, 700},
		[3] = {2, 3,  200, 800},
		[4] = {2, 3,  1000,1000},
		[5] = {2, 4,  300, 700},
		[6] = {2, 3,  500, 500},
		[7] = {0, 0,  0,   0},
		[8] = {0, 0,  0,   0},
	},
};

static const struct persistent_config factory_default = {
	CONFIG_MAGIC,
	CONFIG_VERSION,
	0,                  /* crc32 */
	DEVICE_GROUP_ID,
	DEVICE_TYPE,
	0,                  /* hub_type */
	DEVICE_ROOM_ID,
	/* alarm_cfg filled below */
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
	/* 初始化 factory_default 中的 alarm_cfg */
	struct persistent_config factory = factory_default;
	memcpy(&factory.alarm_cfg, &default_alarm_cfg, sizeof(default_alarm_cfg));
	factory.crc32 = config_crc(&factory);

	int ret;

	ret = read_config(CONFIG_PRIMARY_OFFSET, &current_config);
	if (ret == 0) { config_valid = true; goto done; }

	ret = read_config(CONFIG_BACKUP1_OFFSET, &current_config);
	if (ret == 0) { config_valid = true; goto done; }

	ret = read_config(CONFIG_BACKUP2_OFFSET, &current_config);
	if (ret == 0) { config_valid = true; goto done; }

	/* Factory default */
	memcpy(&current_config, &factory, sizeof(current_config));
	current_config.crc32 = config_crc(&current_config);
	config_valid = true;
	config_store_save();

done:
	SEGGER_RTT_printf(0, "[INFO] Config loaded (group=0x%02X room=%d)\n",
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

	return 0;
}

int config_store_factory_reset(void) {
	struct persistent_config factory = factory_default;
	memcpy(&factory.alarm_cfg, &default_alarm_cfg, sizeof(default_alarm_cfg));
	factory.crc32 = config_crc(&factory);

	write_config(CONFIG_PRIMARY_OFFSET, &factory);
	memcpy(&current_config, &factory, sizeof(current_config));
	current_config.crc32 = config_crc(&current_config);
	config_valid = true;
	SEGGER_RTT_printf(0, "[INFO] Factory reset complete\n");
	return 0;
}

uint8_t config_get_group_id(void)  { return current_config.group_id; }
void    config_set_group_id(uint8_t id) { current_config.group_id = id; config_store_save(); }
uint8_t config_get_hub_type(void)  { return current_config.hub_type; }
void    config_set_hub_type(uint8_t t) { current_config.hub_type = t; config_store_save(); }
uint8_t config_get_room_id(void)   { return current_config.room_id; }
void    config_set_room_id(uint8_t id) { current_config.room_id = id; config_store_save(); }

const struct alarm_config *config_get_alarm_config(void) {
	return &current_config.alarm_cfg;
}

void config_save_alarm_config(const struct alarm_config *cfg) {
	memcpy(&current_config.alarm_cfg, cfg, sizeof(*cfg));
	config_store_save();
	SEGGER_RTT_printf(0, "[INFO] Alarm config saved to flash\n");
}

bool config_is_valid(void) { return config_valid; }
