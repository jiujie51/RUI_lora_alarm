/*
 * 按键状态机 — 4 键消抖 + 事件检测 + 两段式确认
 * 移植自 NCS: k_uptime_get() → millis(), gpio_pin_get() → digitalRead()
 *
 * 按键: Red(P0.24) / Blue(P1.01) / Yellow(P1.02) / Green(P0.25)
 *       上拉电阻, 按下 = LOW
 * 轮询: button_sm_poll() 每 10ms 调用一次 (从 actuatorThread)
 * 确认: 长按 3s → "Hold 2s" → 2s 后触发告警, 提前松手取消
 * 组合: Yellow+Green 5s → Clear All, Green+Blue 5s → 设备开关
 */
#ifndef BUTTON_SM_H
#define BUTTON_SM_H

#include <stdint.h>
#include <stdbool.h>

/* ── 按键 ID (对齐 NCS) ── */
#define BTN_RED    0
#define BTN_BLUE   1
#define BTN_YELLOW 2
#define BTN_GREEN  3
#define BTN_COUNT  4

/* ── 按键事件 ── */
enum btn_event {
	BTN_EVENT_NONE  = 0,
	BTN_EVENT_SHORT = 1,   /* 短按 (30ms~3s 后松开) */
	BTN_EVENT_LONG  = 2,   /* 长按 (按住 ≥3s 发一次) */
	BTN_EVENT_COMBO = 3,   /* 组合键 (2 键同时 ≥5s) */
};

/* ── 回调类型 ── */
typedef void (*btn_callback_t)(uint8_t btn_id, enum btn_event event);

/* ── 公共 API ── */
int  button_sm_init(void);
void button_sm_poll(void);
void button_sm_set_callback(btn_callback_t cb);
bool button_is_pressed(uint8_t btn_id);
enum btn_event button_get_last_event(uint8_t btn_id);

#endif /* BUTTON_SM_H */
