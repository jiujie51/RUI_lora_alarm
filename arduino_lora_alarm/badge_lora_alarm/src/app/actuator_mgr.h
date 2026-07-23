/* 执行器管理器 — 公共接口, Hub 版 (无振动/无Badge UI) */
#ifndef ACTUATOR_MGR_H
#define ACTUATOR_MGR_H

#include <stdint.h>
#include <stdbool.h>

/* LED 模式 */
enum led_mode { LED_MODE_OFF = 0, LED_MODE_ON, LED_MODE_BLINK, LED_MODE_BREATH, LED_MODE_FAST = 4 };

/* 蜂鸣器模式 */
enum buzzer_mode { BUZZER_OFF = 0, BUZZER_ON, BUZZER_PATTERN };

#define BUZZER_VOLUME_MAX      10
#define BUZZER_RED_TIMEOUT_SEC 60
#define BUZZER_OTHER_TIMEOUT_SEC 30

/* LED 颜色 */
struct led_color {
	uint8_t r, g, b;  /* WS2812: app 侧存 RGB, 驱动内部转 GRB */
};

/* 驱动配置条目 (per-priority) */
struct led_cfg { uint8_t r, g, b, mode; uint16_t on_ms, off_ms; };
struct buzzer_cfg { uint8_t mode, volume; uint16_t on_ms, off_ms; };

/* 全局告警配置 (9 priorities) */
struct alarm_config {
	struct led_cfg    led_map[9];
	struct buzzer_cfg buzzer_map[9];
};

int  actuator_mgr_init(void);
int  actuator_mgr_sync(void);
void actuator_mgr_tick(void);
void actuator_show_join_status(int state);

/* 手动覆盖 (CMD 0x05-0x06) */
int  actuator_led_override(uint8_t r, uint8_t g, uint8_t b, uint8_t mode, uint16_t on_ms, uint16_t off_ms);
int  actuator_buzzer_override(uint8_t mode, uint8_t volume, uint16_t on_ms, uint16_t off_ms);
int  actuator_all_off(void);

/* 配置管理 */
const struct alarm_config *actuator_get_config(void);
int  actuator_set_config(const struct alarm_config *cfg);

#endif /* ACTUATOR_MGR_H */
