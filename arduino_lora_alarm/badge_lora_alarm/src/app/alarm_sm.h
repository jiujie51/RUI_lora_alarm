/* 告警状态机 — 公共接口, 直接复制自 NCS */
#ifndef ALARM_SM_H
#define ALARM_SM_H

#include <stdint.h>
#include <stdbool.h>

#define ALARM_PRIO_CODE_RED    0
#define ALARM_PRIO_SHELTER     1
#define ALARM_PRIO_EVACUATE    2
#define ALARM_PRIO_SECURE      3
#define ALARM_PRIO_HOLD        4
#define ALARM_PRIO_CODE_BLUE   5
#define ALARM_PRIO_CODE_YELLOW 6
#define ALARM_PRIO_ALL_CLEAR   7
#define ALARM_PRIO_NORMAL      8
#define ALARM_PRIO_MAX         9

#define ALARM_TYPE_RED        0x01
#define ALARM_TYPE_BLUE       0x02
#define ALARM_TYPE_YELLOW     0x03
#define ALARM_TYPE_GREEN      0x04
#define ALARM_TYPE_HOLD       0x05
#define ALARM_TYPE_SECURE     0x06
#define ALARM_TYPE_EVACUATE   0x07
#define ALARM_TYPE_SHELTER    0x08

#define ALARM_SRC_LORAWAN     0
#define ALARM_SRC_BADGE_BTN   1
#define ALARM_SRC_LOCAL       2

struct active_alarm {
	uint8_t  type;
	uint8_t  priority;
	uint8_t  source;
	uint32_t set_time;
};

int  alarm_sm_init(void);
int  alarm_sm_set(uint8_t alarm_type, uint8_t source);
int  alarm_sm_clear(uint8_t alarm_type);
int  alarm_sm_all_clear(void);
int  alarm_sm_clear_all(void);
uint8_t alarm_sm_current_priority(void);
uint8_t alarm_sm_current_type(void);
bool alarm_sm_is_active(void);
int  alarm_sm_get_active(struct active_alarm *list, uint8_t max_len);
uint8_t alarm_type_to_priority(uint8_t alarm_type);
uint8_t alarm_priority_to_type(uint8_t priority);

#endif /* ALARM_SM_H */
