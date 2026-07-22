/* LoRaWAN 入网状态 (main.ino 提供, 各模块共用) — 直接复制自 NCS */
#ifndef JOIN_STATE_H
#define JOIN_STATE_H

enum join_state {
	JOIN_STATE_OFFLINE = 0,
	JOIN_STATE_JOINING,
	JOIN_STATE_WAIT,
	JOIN_STATE_JOINED,
	JOIN_STATE_FAILED,
};

int get_join_state(void);

#endif /* JOIN_STATE_H */
