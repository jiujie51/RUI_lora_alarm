#include <stdint.h>
/*
 * 执行器管理器 (Badge 版) — 告警类型 → 硬件时序命令调度
 * Badge: 3 颗 RGB LED (PWM) + 蜂鸣器 + 振动马达
 */
#include <Arduino.h>
#include <string.h>
#include "alarm_sm.h"
#include "actuator_mgr.h"
#include "join_state.h"
#include "../drv/led_strip.h"
#include "../drv/buzzer_pwm.h"
#include "../config/config_store.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 入网状态显示方式 ──
 * 1 = LED 显示 (蓝闪=JOINING, 红快闪=FAILED, 灭=JOINED)
 * 0 = 日志打印
 */
#define JOIN_STATUS_USE_LED  0


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
		[0] = {2, 5,  120, 80 },               /* Code Red: 急促 5Hz, vol=5 */
		[1] = {2, 5,  500, 500},                /* Shelter */
		[2] = {2, 5,  300, 700},                /* Evacuate */
		[3] = {2, 3,  200, 800},                /* Secure */
		[4] = {2, 3,  1000,1000},               /* Hold */
		[5] = {2, 4,  300, 700},                /* Code Blue: 中速, vol=4 */
		[6] = {2, 3,  500, 500},                /* Code Yellow: 慢速, vol=3 */
		[7] = {0, 0,  0,   0},                  /* All Clear */
		[8] = {0, 0,  0,   0},                  /* Normal */
	},
};

static struct alarm_config config;

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

int actuator_mgr_init(void) {
	/* 从 Flash 恢复 CMD 0x04 告警配置 (若无效则用 default) */
	const struct alarm_config *saved = config_get_alarm_config();
	if (saved && saved->led_map[0].r != 0) {
		/* 简单校验: P0 Red 的 R 分量非零 = 已配置过 */
		memcpy(&config, saved, sizeof(config));
		SEGGER_RTT_printf(0, "[INFO] Alarm config loaded from flash\n");
	} else {
		memcpy(&config, &default_config, sizeof(config));
	}

	pinMode(MOTOR_PIN, OUTPUT);
	digitalWrite(MOTOR_PIN, LOW);
	SEGGER_RTT_printf(0, "[INFO] Actuator manager initialized (Badge)\n");
	return 0;
}

int actuator_mgr_sync(void) {
	if (override_active) return 0;

	uint8_t new_prio = alarm_sm_current_priority();
	uint8_t new_type = alarm_sm_current_type();

	if (new_prio == current_alarm_priority && new_type == current_alarm_type)
		goto tick;

	SEGGER_RTT_printf(0, "[INFO] Actuator sync: prio=%d type=%d (was prio=%d)\n",
		new_prio, new_type, current_alarm_priority);

	current_alarm_priority = new_prio;
	current_alarm_type     = new_type;

	if (new_prio == ALARM_PRIO_NORMAL) {
		apply_led(8); apply_buzzer(8);
		digitalWrite(MOTOR_PIN, LOW);
	} else {
		apply_led(new_prio); apply_buzzer(new_prio);
		digitalWrite(MOTOR_PIN, HIGH);
	}

tick:
	led_strip_tick();
	buzzer_pwm_tick();
	return 0;
}

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

int actuator_vibration_override(uint8_t mode, uint16_t on_ms, uint16_t off_ms) {
	override_active = true;
	switch (mode) {
	case 0:  /* 关 */
		digitalWrite(MOTOR_PIN, LOW);
		break;
	case 1:  /* 常震 */
		digitalWrite(MOTOR_PIN, HIGH);
		break;
	case 2:  /* 间歇 (由 tick 周期性处理 — 简化: 直接常震) */
		/* TODO: 实现间歇震动 pattern (需要 tick 处理或定时器) */
		digitalWrite(MOTOR_PIN, HIGH);
		SEGGER_RTT_printf(0, "[INFO] Vibration pattern mode (interval), use continuous for now\n");
		break;
	default:
		return -22;
	}
	return 0;
}

int actuator_all_off(void) {
	override_active = false;
	current_alarm_priority = ALARM_PRIO_NORMAL;
	led_strip_off();
	buzzer_pwm_off();
	digitalWrite(MOTOR_PIN, LOW);
	return 0;
}

void actuator_mgr_tick(void) {
	int js = get_join_state();
	if (js != last_join_state) {
		last_join_state = js;
		SEGGER_RTT_printf(0, "[INFO] Join state changed: %d\n", js);
		if (current_alarm_priority == ALARM_PRIO_NORMAL) {
			actuator_show_join_status(js);
		}
	}

	if (!override_active) {
		actuator_mgr_sync();
	} else {
		led_strip_tick();
		buzzer_pwm_tick();
	}
}

/* ── 入网状态指示 ── */
static const char *join_state_names[] = {
	"OFFLINE", "JOINING", "WAIT", "JOINED", "FAILED"
};

void actuator_show_join_status(int state) {
	if (current_alarm_priority != ALARM_PRIO_NORMAL || override_active) return;

#if JOIN_STATUS_USE_LED
	struct led_color blue = {0, 0, 64};
	struct led_color red  = {64, 0, 0};

	switch (state) {
	case JOIN_STATE_JOINING:
		led_strip_set_mode(LED_MODE_BLINK, blue, 1000, 1000);
		break;
	case JOIN_STATE_WAIT:
		led_strip_off();
		break;
	case JOIN_STATE_JOINED:
		led_strip_off();
		break;
	case JOIN_STATE_FAILED:
		led_strip_set_mode(LED_MODE_FAST, red, 300, 300);
		break;
	default: break;
	}
#else
	const char *name = (state >= 0 && state <= 4)
		? join_state_names[state] : "UNKNOWN";
	SEGGER_RTT_printf(0, "[INFO] Join status: %s (%d)\n", name, state);
#endif
}

const struct alarm_config *actuator_get_config(void) { return &config; }

int actuator_set_config(const struct alarm_config *cfg) {
	memcpy(&config, cfg, sizeof(config));
	return 0;
}
