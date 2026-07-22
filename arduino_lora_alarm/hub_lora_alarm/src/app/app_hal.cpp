#include <stdint.h>
/*
 * LoRaWAN 硬件抽象层 — RUI3 api.lorawan 封装
 * 新建模块, 替代 NCS hal_sx1262.c / lorawan_classb.c / lorawan_mc.c
 *
 * RUI3 自动管理: DevNonce, DevAddr, NwkSKey, AppSKey, FCnt, 入网模式, 频段, Class
 * 应用层只需: 凭证设置 → join → send/recv callback
 */
#include <Arduino.h>
#include "app_hal.h"
#include "../app/join_state.h"
#include "../proto/proto_internal.h"
#include "../boards/hub/board.h"
#include "nrf_log.h"


static lora_downlink_cb_t g_downlink_cb = NULL;
static int join_state_val = JOIN_STATE_OFFLINE;
static uint8_t join_attempt = 0;
static uint32_t next_join_ms = 0;
static bool beacon_locked = false;

static const uint32_t join_backoff_ms[] = {10000, 20000, 40000, 60000, 60000, 60000};

/* ── RUI3 下行回调 ── */
static void ruiv3_recv_cb(SERVICE_LORA_RECEIVE_T *data) {
	if (data->BufferSize > 0 && g_downlink_cb) {
		g_downlink_cb(data->Port, data->Buffer, data->BufferSize);
	}
}

/* ── RUI3 入网回调 ── */
static void ruiv3_join_cb(int32_t status) {
	if (status == 0) {
		join_state_val = JOIN_STATE_JOINED;
		NRF_LOG_INFO("LoRaWAN joined successfully");
	} else if (status == RAK_LORAMAC_STATUS_BEACON_LOCKED) {
		beacon_locked = true;
		NRF_LOG_INFO("Class B beacon locked");
	} else if (status == RAK_LORAMAC_STATUS_BEACON_LOST) {
		beacon_locked = false;
		NRF_LOG_WARNING("Class B beacon lost");
	}
}

/* ── 初始化 ── */
void app_hal_lorawan_init(void) {
	if (api.lorawan.nwm.get() != 1) {
		NRF_LOG_INFO("Set Node device work mode %s\r\n",
				api.lorawan.nwm.set() ? "Success" : "Fail");
		api.system.reboot();
	}

	uint8_t dev_eui[8]  = OTAA_DEVEUI;
	uint8_t join_eui[8] = OTAA_APPEUI;
	uint8_t app_key[16] = OTAA_APPKEY;

	api.lorawan.deui.set(dev_eui, 8);
	api.lorawan.appeui.set(join_eui, 8);
	api.lorawan.appkey.set(app_key, 16);

	api.lorawan.band.set(OTAA_BAND);
	api.lorawan.deviceClass.set(RAK_LORA_CLASS_B);
	api.lorawan.njm.set(RAK_LORA_OTAA);

	api.lorawan.adr.set(DEVICE_TYPE == 1);
	api.lorawan.rety.set(1);
	api.lorawan.cfm.set(0);

	api.lorawan.registerRecvCallback(ruiv3_recv_cb);
	api.lorawan.registerJoinCallback(ruiv3_join_cb);

	NRF_LOG_INFO("LoRaWAN HAL initialized (band=%d, class=B)", OTAA_BAND);
}

/* ── 下行回调注册 ── */
void app_hal_set_downlink_cb(lora_downlink_cb_t cb) {
	g_downlink_cb = cb;
}

/* ── 入网状态 ── */
bool app_hal_is_joined(void) {
	return api.lorawan.njs.get();
}

/* ── 入网状态机 tick (由 loraThread 协程调用) ── */
void app_hal_join_tick(void) {
	if (api.lorawan.njs.get()) {
		if (join_state_val != JOIN_STATE_JOINED) {
			join_state_val = JOIN_STATE_JOINED;
			join_attempt = 0;
		}
		return;
	}

	uint32_t now = millis();

	if (join_state_val != JOIN_STATE_JOINING && join_state_val != JOIN_STATE_WAIT) {
		join_state_val = JOIN_STATE_JOINING;
		join_attempt = 0;
		next_join_ms = now;
	}

	if (now >= next_join_ms) {
		uint8_t idx = (join_attempt < 6) ? join_attempt : 5;
		join_state_val = JOIN_STATE_JOINING;
		api.lorawan.join();

		if (join_attempt < 6) join_attempt++;
		next_join_ms = now + join_backoff_ms[idx];

		if (join_attempt >= 6) join_state_val = JOIN_STATE_FAILED;
	}
}

int app_hal_get_join_state(void) { return join_state_val; }

bool app_hal_is_beacon_locked(void) { return beacon_locked; }

/* ── 发送 (beacon lock 未就绪时阻塞上行, 保护 Class B 状态机) ── */
bool app_hal_send(uint8_t fport, const uint8_t *data, uint8_t len, bool confirmed) {
	if (!api.lorawan.njs.get()) {
		NRF_LOG_WARNING("TX blocked: not joined");
		return false;
	}
	if (!beacon_locked) {
		NRF_LOG_WARNING("TX blocked: beacon not locked");
		return false;
	}

	for (int attempt = 0; attempt < 3; attempt++) {
		if (api.lorawan.send(len, (uint8_t *)data, fport, confirmed, 3)) {
			return true;
		}
		NRF_LOG_WARNING("TX attempt %d/3 failed, retry in 2s...", attempt + 1);
		delay(2000);
	}
	return false;
}

/* ── 多播组注册 ── */
void app_hal_setup_multicast(void) {
	/* TODO: 从 ChirpStack 下发 McGroup 后调用 api.lorawan.addmulc() */
}
