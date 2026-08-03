#include <stdint.h>
/*
 * 按键状态机 — 4-pass 消抖 + 事件检测
 * 移植自 NCS: k_uptime_get() → millis(), gpio_pin_get() → digitalRead()
 *
 * Pass 1: 消抖 (30ms = 3 次 10ms 采样)
 * Pass 2: 多键跟踪 (粘性 multi_touch 标记)
 * Pass 3: 组合键检测 (2 键 5s → BTN_EVENT_COMBO)
 * Pass 4: 单键事件 (按住≥3s → LONG, 松开<3s → SHORT)
 */
#include <Arduino.h>
#include <string.h>
#include "button_sm.h"
#include "../boards/badge/board.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/* ── 时序常量 ── */
#define DEBOUNCE_THRESHOLD  (BUTTON_DEBOUNCE_MS / 10)  /* 3 */
#define LONG_MIN_MS         BUTTON_LONG_PRESS_MS       /* 3000 */
#define COMBO_MIN_MS        5000

/* ── 引脚映射 (按 BTN_RED=0, BTN_BLUE=1, BTN_YELLOW=2, BTN_GREEN=3) ── */
static const uint8_t btn_pins[BTN_COUNT] = {
	BUTTON_RED_PIN,    /* P0_24 */
	BUTTON_BLUE_PIN,   /* P1_01 */
	BUTTON_YELLOW_PIN, /* P1_02 */
	BUTTON_GREEN_PIN,  /* P0_25 */
};

/* ── 组合键定义 ── */
struct combo_def {
	uint8_t a, b;
	uint8_t report_id;
};
static const struct combo_def combos[] = {
	{ BTN_BLUE,  BTN_YELLOW, BTN_BLUE  }, /* Blue+Yellow: Reset (清除所有告警) */
	{ BTN_GREEN, BTN_BLUE,   BTN_GREEN  }, /* Green+Blue: Device toggle (禁用/启用) */
	{ BTN_GREEN, BTN_YELLOW, BTN_YELLOW }, /* Green+Yellow: (预留) */
};
#define COMBO_COUNT (sizeof(combos) / sizeof(combos[0]))

/* ── 每键状态 ── */
struct btn_state_t {
	bool     debounced;       /* 消抖后的稳定电平 (true=按下) */
	bool     long_fired;      /* 本次按下已触发 LONG */
	bool     multi_touch;     /* 粘性: 曾被其他键同时按下 */
	uint32_t press_start_ms;  /* 按下时刻 */
	enum btn_event last_event;
	uint8_t  debounce_cnt;    /* 连续不匹配计数 (0~DEBOUNCE_THRESHOLD) */
};

static struct btn_state_t btns[BTN_COUNT];
static btn_callback_t g_callback;

/* ── 全局多键状态 ── */
static uint8_t pressed_count;
static bool    combo_latched;

/* ── 按键名称 (日志用) ── */
static const char *btn_names[BTN_COUNT] = { "RED", "BLUE", "YELLOW", "GREEN" };

/* ── 初始化 ── */
int button_sm_init(void) {
	SEGGER_RTT_printf(0, "[BTN] init start, %d buttons\n", BTN_COUNT);
	for (int i = 0; i < BTN_COUNT; i++) {
		SEGGER_RTT_printf(0, "[BTN] pinMode btn[%d] pin=%d\n", i, btn_pins[i]);
		pinMode(btn_pins[i], INPUT_PULLUP);
		memset(&btns[i], 0, sizeof(btns[i]));
	}
	pressed_count = 0;
	combo_latched = false;
	g_callback = NULL;

	SEGGER_RTT_printf(0, "[BTN] init done\n");
	SEGGER_RTT_printf(0, "[INFO] Button SM initialized (4 keys, %dms debounce, %dms long)\n",
		BUTTON_DEBOUNCE_MS, BUTTON_LONG_PRESS_MS);
	return 0;
}

void button_sm_set_callback(btn_callback_t cb) {
	g_callback = cb;
}

/* ── 读取原始电平 (上拉: LOW=按下) ── */
static inline bool raw_pressed(int i) {
	return digitalRead(btn_pins[i]) == LOW;
}

/* ── 分发事件 ── */
static void fire_event(uint8_t id, enum btn_event evt) {
	btns[id].last_event = evt;
	if (g_callback) g_callback(id, evt);
}

/* ══════════════════════════════════════════════════════════
 * button_sm_poll() — 4-pass 轮询 (每 10ms 调用)
 * ══════════════════════════════════════════════════════════ */
void button_sm_poll(void) {
	uint32_t now = millis();
	bool release_edge[BTN_COUNT] = { false };
	uint32_t held[BTN_COUNT] = { 0 };

	/* ── Pass 1: 消抖 + 边沿检测 ── */
	for (int i = 0; i < BTN_COUNT; i++) {
		bool raw = raw_pressed(i);

		if (raw != btns[i].debounced) {
			btns[i].debounce_cnt++;
			if (btns[i].debounce_cnt >= DEBOUNCE_THRESHOLD) {
				btns[i].debounced = raw;
				btns[i].debounce_cnt = 0;

				if (raw) {
					/* 按下边沿 */
					btns[i].press_start_ms = now;
					btns[i].long_fired = false;
					btns[i].multi_touch = false;
					btns[i].last_event = BTN_EVENT_NONE;
				} else {
					/* 松开边沿 */
					release_edge[i] = true;
					held[i] = now - btns[i].press_start_ms;
					btns[i].last_event = BTN_EVENT_NONE;
				}
			}
		} else {
			btns[i].debounce_cnt = 0;
		}
	}

	/* ── Pass 2: 多键跟踪 ── */
	pressed_count = 0;
	for (int i = 0; i < BTN_COUNT; i++)
		if (btns[i].debounced) pressed_count++;

	if (pressed_count == 0) {
		combo_latched = false;
	} else if (pressed_count >= 2) {
		for (int i = 0; i < BTN_COUNT; i++)
			if (btns[i].debounced) btns[i].multi_touch = true;
	}

	/* ── Pass 3: 组合键检测 (5s) ── */
	if (!combo_latched && pressed_count == 2) {
		for (size_t c = 0; c < COMBO_COUNT; c++) {
			int a = combos[c].a, b = combos[c].b;
			if (btns[a].debounced && btns[b].debounced
			    && (now - btns[a].press_start_ms) >= COMBO_MIN_MS
			    && (now - btns[b].press_start_ms) >= COMBO_MIN_MS) {
				combo_latched = true;
				SEGGER_RTT_printf(0, "[INFO] Combo: %s+%s %ds\n", btn_names[a], btn_names[b], COMBO_MIN_MS / 1000);
				fire_event(combos[c].report_id, BTN_EVENT_COMBO);
				break;
			}
		}
	}

	/* ── Pass 4: 单键事件 ── */
	for (int i = 0; i < BTN_COUNT; i++) {
		/* LONG: 按住 ≥3s, 单键, 非多键, 未触发过 */
		if (btns[i].debounced && pressed_count == 1 && !btns[i].multi_touch
		    && !btns[i].long_fired
		    && (now - btns[i].press_start_ms) >= LONG_MIN_MS) {
			btns[i].long_fired = true;
			SEGGER_RTT_printf(0, "[INFO] Btn[%d] %s LONG (%lums)\n", i, btn_names[i],
				now - btns[i].press_start_ms);
			fire_event(i, BTN_EVENT_LONG);
		}

		/* SHORT: 松开, 非多键, 未触发 LONG, 30ms~3s */
		if (release_edge[i] && !btns[i].multi_touch && !btns[i].long_fired
		    && held[i] >= BUTTON_DEBOUNCE_MS && held[i] < LONG_MIN_MS) {
			SEGGER_RTT_printf(0, "[INFO] Btn[%d] %s SHORT (%lums)\n", i, btn_names[i], held[i]);
			fire_event(i, BTN_EVENT_SHORT);
		}
	}
}

/* ── 辅助函数 ── */
bool button_is_pressed(uint8_t id) {
	return (id < BTN_COUNT) ? btns[id].debounced : false;
}

enum btn_event button_get_last_event(uint8_t id) {
	return (id < BTN_COUNT) ? btns[id].last_event : BTN_EVENT_NONE;
}
