/*
 * 执行器管理器 (Hub 版) — 告警类型 → 硬件时序命令调度
 * 移植自 NCS: 移除 Badge/BLE/CONFIG_* 依赖, Hub 用 WS2812 灯带 + nrfx_pwm 蜂鸣器
 */
#include <Arduino.h>
#include <string.h>
#include "alarm_sm.h"
#include "actuator_mgr.h"
#include "join_state.h"
#include "../drv/led_strip.h"
#include "../drv/buzzer_pwm.h"
#include "../debug_macros.h"

#define TAG "actuator"

/* ── 当前激活的执行器状态 ── */
static uint8_t  current_alarm_type     = 0;
static uint8_t  current_alarm_priority = 8;
static bool     override_active        = false;
static int      last_join_state        = -1;

/* ── 默认告警配置 ── */
static const struct alarm_config default_config = {
	.led_map = {
		[0] = {255, 0,   0,   4, 300,  300},   /* P0: Code Red */
		[1] = {180, 0,   255, 2, 500,  500},   /* P1: Shelter */
		[2] = {255, 120, 0,   2, 500,  500},   /* P2: Evacuate */
		[3] = {180, 0,   255, 1, 0,    0},     /* P3: Secure */
		[4] = {180, 0,   255, 2, 1000, 1000},  /* P4: Hold */
		[5] = {0,   80,  255, 2, 500,  500},   /* P5: Code Blue */
		[6] = {255, 220, 0,   2, 500,  500},   /* P6: Code Yellow */
		[7] = {0,   255, 60,  1, 0,    0},     /* P7: All Clear */
		[8] = {0,   0,   0,   0, 0,    0},     /* P8: Normal */
	},
	.buzzer_map = {
		[0] = {1, 10, 0,   0},                  /* Code Red: ON, 60s */
		[1] = {2, 8,  500, 500},                /* Shelter */
		[2] = {2, 8,  300, 700},                /* Evacuate */
		[3] = {2, 5,  200, 800},                /* Secure */
		[4] = {2, 5,  1000,1000},               /* Hold */
		[5] = {2, 6,  500, 500},                /* Code Blue */
		[6] = {2, 4,  500, 500},                /* Code Yellow */
		[7] = {0, 0,  0,   0},                  /* All Clear */
		[8] = {0, 0,  0,   0},                  /* Normal */
	},
};

static struct alarm_config config;

/* ── 应用 LED 配置 (Hub: WS2812 GRB 序) ── */
static int apply_led(uint8_t prio) {
	if (prio >= 9) return -22;
	const struct alarm_config *cfg = &config;
	struct led_color c = {cfg->led_map[prio].r, cfg->led_map[prio].g, cfg->led_map[prio].b};
	uint8_t  m   = cfg->led_map[prio].mode;
	uint16_t on  = cfg->led_map[prio].on_ms;
	uint16_t off = cfg->led_map[prio].off_ms;

	switch (m) {
	case 0: return led_strip_off();
	case 1: return led_strip_set_mode(LED_MODE_ON, c, 0, 0);
	case 2: return led_strip_set_mode(LED_MODE_BLINK, c, on, off);
	case 4: return led_strip_set_mode(LED_MODE_FAST, c, on, off);
	default: return -22;
	}
}

/* ── 应用蜂鸣器配置 ── */
static int apply_buzzer(uint8_t prio) {
	if (prio >= 9) return -22;
	const struct alarm_config *cfg = &config;
	uint8_t m = cfg->buzzer_map[prio].mode;
	uint8_t v = cfg->buzzer_map[prio].volume;

	switch (m) {
	case 0: return buzzer_pwm_off();
	case 1: return buzzer_pwm_set(BUZZER_ON, v, 0, 0);
	case 2: return buzzer_pwm_set(BUZZER_PATTERN, v,
			cfg->buzzer_map[prio].on_ms, cfg->buzzer_map[prio].off_ms);
	default: return -22;
	}
}

/* ── 初始化 ── */
int actuator_mgr_init(void) {
	memcpy(&config, &default_config, sizeof(config));
	LOG_INFO(TAG, "Actuator manager initialized (Hub, default config)");
	return 0;
}

/* ── 同步 alarm_sm → 执行器 ── */
int actuator_mgr_sync(void) {
	if (override_active) return 0;

	uint8_t new_prio = alarm_sm_current_priority();
	uint8_t new_type = alarm_sm_current_type();

	if (new_prio == current_alarm_priority && new_type == current_alarm_type)
		goto tick;

	LOG_INFO(TAG, "Actuator sync: prio=%d type=%d (was prio=%d)",
		new_prio, new_type, current_alarm_priority);

	current_alarm_priority = new_prio;
	current_alarm_type     = new_type;

	if (new_prio == ALARM_PRIO_NORMAL) {
		apply_led(8); apply_buzzer(8);
	} else {
		apply_led(new_prio); apply_buzzer(new_prio);
	}

tick:
	led_strip_tick();
	buzzer_pwm_tick();
	return 0;
}

/* ── 手动覆盖 (CMD 0x05-0x06) ── */
int actuator_led_override(uint8_t r, uint8_t g, uint8_t b,
			  uint8_t mode, uint16_t on_ms, uint16_t off_ms) {
	if (mode > LED_MODE_FAST) return -22;
	override_active = true;
	struct led_color c = {r, g, b};
	return led_strip_set_mode((enum led_mode)mode, c, on_ms, off_ms);
}

int actuator_buzzer_override(uint8_t mode, uint8_t volume,
			     uint16_t on_ms, uint16_t off_ms) {
	if (mode > BUZZER_PATTERN) return -22;
	override_active = true;
	return buzzer_pwm_set((enum buzzer_mode)mode, volume, on_ms, off_ms);
}

int actuator_all_off(void) {
	override_active = false;
	current_alarm_priority = ALARM_PRIO_NORMAL;
	led_strip_off();
	buzzer_pwm_off();
	return 0;
}

/* ── 周期性 tick (由 actuatorThread 每 10ms 调用) ── */
void actuator_mgr_tick(void) {
	int js = get_join_state();
	if (js != last_join_state && current_alarm_priority == ALARM_PRIO_NORMAL) {
		last_join_state = js;
		actuator_show_join_status(js);
	}

	if (!override_active) {
		actuator_mgr_sync();
	} else {
		led_strip_tick();
		buzzer_pwm_tick();
	}
}

void actuator_show_join_status(int state) {
	if (current_alarm_priority != ALARM_PRIO_NORMAL || override_active) return;

	struct led_color off  = {0, 0, 0};
	struct led_color blue = {0, 0, 64};
	struct led_color red  = {64, 0, 0};

	switch (state) {
	case 1: /* JOINING — 蓝慢闪 */
		led_strip_set_mode(LED_MODE_BLINK, blue, 1000, 1000);
		break;
	case 2: /* WAIT — 灭 */
		led_strip_off();
		break;
	case 3: /* JOINED — 灭 */
		led_strip_off();
		break;
	case 4: /* FAILED — 红快闪 */
		led_strip_set_mode(LED_MODE_FAST, red, 300, 300);
		break;
	default: break;
	}
}

/* ── 配置管理 ── */
const struct alarm_config *actuator_get_config(void) { return &config; }

int actuator_set_config(const struct alarm_config *cfg) {
	memcpy(&config, cfg, sizeof(config));
	return 0;
}
