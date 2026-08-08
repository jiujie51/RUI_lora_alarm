#include <stdint.h>
/*
 * LoRaWAN 硬件抽象层 — Hub 版 (RUI3 api.lorawan 封装)
 *
 * 与 Badge 共用: beacon lock, 多播, 发送重试逻辑
 * Hub 固定设备, ADR 开启
 */
#include <Arduino.h>
#include "app_hal.h"
#include "../app/join_state.h"
#include "../proto/proto_internal.h"
#include "../boards/hub/board.h"
#include "../config/device_identity.h"

extern "C" int SEGGER_RTT_printf(unsigned, const char*, ...);

/*
 * RUI3 内部 Class B 状态 (service_lora.c)
 * service_lora_get_class_b_state() 已在 service_lora.h 中声明 (经 RAKLorawan.h → Arduino.h 可见)
 * 返回值 SERVICE_LORA_CLASS_B_STATE: S0=0/S1=1/S2=2/S3=3/COMPLETED=4
 */

static const char *cls_b_state_name(int s) {
	switch (s) {
		case 0:  return "S0_DeviceTime";
		case 1:  return "S1_BeaconSearch";
		case 2:  return "S2_BeaconLocked";
		case 3:  return "S3_BeaconFailed";
		case 4:  return "COMPLETED";
		default: return "?";
	}
}

static lora_downlink_cb_t g_downlink_cb = NULL;
static int join_state_val = JOIN_STATE_OFFLINE;
static uint8_t join_attempt = 0;
static uint32_t next_join_ms = 0;
static volatile bool tx_busy = false;  /* true = MAC 正在处理 TX+RX, 禁止新发送 */

/* ── Beacon 状态机 ── */
static beacon_state_t bcn_state = BCN_IDLE;
static uint32_t bcn_phase_start_ms = 0;
static uint32_t bcn_next_retry_ms  = 0;

#define BCN_SEARCH_TIMEOUT_MS   130000   /* US915 beacon period 128s + margin */
#define BCN_RETRY_INTERVAL_MS   300000   /* 5min between retries from fallback */

static const char *bcn_state_name(int s) {
	switch (s) {
		case BCN_IDLE:      return "IDLE";
		case BCN_SEARCHING: return "SEARCHING";
		case BCN_LOCKED:    return "LOCKED";
		case BCN_FALLBACK:  return "FALLBACK";
		case BCN_RETRY:     return "RETRY";
		default:            return "?";
	}
}
/*
 * RUI3 不会通过 join callback 通知 beacon 事件 (已验证)，
 * 改用 api.lorawan.btime.get() 判断 beacon 是否已收到。
 * btime > 0 表示至少收到过一次 beacon → Class B beacon 已同步。
 */
static inline bool rx_beacon(void) {
	return api.lorawan.btime.get() > 0;
}

static const uint32_t join_backoff_ms[] = {10000, 20000, 40000, 60000, 60000, 60000};

/* ── RUI3 下行回调 ── */
static void ruiv3_recv_cb(SERVICE_LORA_RECEIVE_T *data) {
	SEGGER_RTT_printf(0, "[RX] Port=%u Size=%u DR=%u RSSI=%d SNR=%d FCnt=%lu\r\n",
		data->Port, data->BufferSize, data->RxDatarate,
		data->Rssi, data->Snr, (unsigned long)data->DownLinkCounter);

	if (data->BufferSize > 0) {
		/* Hex dump: 16 bytes per line */
		SEGGER_RTT_printf(0, "[RX] ");
		for (uint8_t i = 0; i < data->BufferSize; i++) {
			SEGGER_RTT_printf(0, "%02X ", data->Buffer[i]);
		}
		SEGGER_RTT_printf(0, "\r\n");

		if (g_downlink_cb) {
			g_downlink_cb(data->Port, data->Buffer, data->BufferSize);
		}
	}
}

/* ── RUI3 入网回调 ──
 * 注意: RUI3 service_lora.c 仅在 join 成功/失败时调用此回调。
 * BEACON_LOCKED/LOST/NOT_FOUND 不会通过此回调分发 (已验证)。
 * Beacon 状态使用 api.lorawan.btime.get() > 0 判断。
 */
static void ruiv3_join_cb(int32_t status) {
	switch (status) {
	case 0: /* RAK_LORAMAC_STATUS_OK — 入网成功 */
		join_state_val = JOIN_STATE_JOINED;
		SEGGER_RTT_printf(0, "[INFO] LoRaWAN joined successfully\r\n");
		break;
	case RAK_LORAMAC_STATUS_JOIN_FAIL:
		SEGGER_RTT_printf(0, "[ERROR] LoRaWAN join failed!\r\n");
		break;
	case RAK_LORAMAC_STATUS_RX1_TIMEOUT:
	case RAK_LORAMAC_STATUS_RX2_TIMEOUT:
		/* 入网: 一个窗口收到 Join-Accept 后另一窗口自然超时, 属正常行为 */
		break;
	/* 以下状态 RUI3 不会通过 join_cb 分发, 保留用于未来兼容 */
	case RAK_LORAMAC_STATUS_BEACON_LOCKED:
	case RAK_LORAMAC_STATUS_BEACON_LOST:
	case RAK_LORAMAC_STATUS_BEACON_NOT_FOUND:
	default:
		SEGGER_RTT_printf(0, "[WARN] join_cb status: %ld (0x%lX)\r\n",
			status, status);
		break;
	}
}

/* ── RUI3 发送回调 ── */
static void ruiv3_send_cb(int32_t status) {
	tx_busy = false;  /* TX+RX 周期结束, 允许下一次发送 */

	switch (status) {
	case RAK_LORAMAC_STATUS_OK:
		/* TX success — silent in normal operation */
		break;
	case RAK_LORAMAC_STATUS_ERROR:
		SEGGER_RTT_printf(0, "[WARN] send_cb: TX ERROR\r\n");
		break;
	case RAK_LORAMAC_STATUS_TX_TIMEOUT:
		SEGGER_RTT_printf(0, "[WARN] send_cb: TX TIMEOUT\r\n");
		break;
	case RAK_LORAMAC_STATUS_RX1_TIMEOUT:
		SEGGER_RTT_printf(0, "[WARN] send_cb: RX1 TIMEOUT (beacon/ping-slot issue?)\r\n");
		break;
	case RAK_LORAMAC_STATUS_RX2_TIMEOUT:
		SEGGER_RTT_printf(0, "[WARN] send_cb: RX2 TIMEOUT\r\n");
		break;
	case RAK_LORAMAC_STATUS_RX1_ERROR:
		SEGGER_RTT_printf(0, "[WARN] send_cb: RX1 ERROR\r\n");
		break;
	case RAK_LORAMAC_STATUS_RX2_ERROR:
		SEGGER_RTT_printf(0, "[WARN] send_cb: RX2 ERROR\r\n");
		break;
	case RAK_LORAMAC_STATUS_TX_DR_PAYLOAD_SIZE_ERROR:
		SEGGER_RTT_printf(0, "[WARN] send_cb: TX DR PAYLOAD SIZE ERROR\r\n");
		break;
	case RAK_LORAMAC_STATUS_DOWNLINK_REPEATED:
		SEGGER_RTT_printf(0, "[WARN] send_cb: DOWNLINK REPEATED (FCnt mismatch)\r\n");
		break;
	case RAK_LORAMAC_STATUS_DOWNLINK_TOO_MANY_FRAMES_LOSS:
		SEGGER_RTT_printf(0, "[WARN] send_cb: TOO MANY FRAMES LOSS\r\n");
		break;
	case RAK_LORAMAC_STATUS_ADDRESS_FAIL:
		SEGGER_RTT_printf(0, "[WARN] send_cb: ADDRESS FAIL\r\n");
		break;
	case RAK_LORAMAC_STATUS_MIC_FAIL:
		SEGGER_RTT_printf(0, "[WARN] send_cb: MIC FAIL\r\n");
		break;
	case RAK_LORAMAC_STATUS_MULTICAST_FAIL:
		SEGGER_RTT_printf(0, "[WARN] send_cb: MULTICAST FAIL\r\n");
		break;
	default:
		SEGGER_RTT_printf(0, "[WARN] send_cb unhandled status: %ld (0x%lX)\r\n",
			status, status);
		break;
	}
}

/* ── 初始化 ── */
void app_hal_lorawan_init(void) {
	if (api.lorawan.nwm.get() != 1) {
		SEGGER_RTT_printf(0, "[INFO] Set Node device work mode %s\r\n",
				api.lorawan.nwm.set() ? "Success" : "Fail");
		api.system.reboot();
	}

	/* 从 flash 读取设备身份 (BLE MAC + LoRaWAN 凭证) */
	const struct device_identity *id = device_identity_get();
	if (device_identity_is_valid()) {
		api.lorawan.deui.set((uint8_t *)id->dev_eui, 8);
		api.lorawan.appeui.set((uint8_t *)id->app_eui, 8);
		api.lorawan.appkey.set((uint8_t *)id->app_key, 16);
	} else {
		/* fallback: 编译期默认凭证 */
		uint8_t dev_eui[8]  = OTAA_DEVEUI;
		uint8_t join_eui[8] = OTAA_APPEUI;
		uint8_t app_key[16] = OTAA_APPKEY;
		api.lorawan.deui.set(dev_eui, 8);
		api.lorawan.appeui.set(join_eui, 8);
		api.lorawan.appkey.set(app_key, 16);
	}

	api.lorawan.band.set(OTAA_BAND);
	api.lorawan.deviceClass.set(RAK_LORA_CLASS_B);
	api.lorawan.pgslot.set(2);
	api.lorawan.njm.set(RAK_LORA_OTAA);

	/* 入网前设置 (与旧版 hub 一致, 保证入网兼容性) */
	api.lorawan.adr.set(true);
	api.lorawan.rety.set(1);
	api.lorawan.cfm.set(0);

	api.lorawan.registerRecvCallback(ruiv3_recv_cb);
	api.lorawan.registerJoinCallback(ruiv3_join_cb);
	api.lorawan.registerSendCallback(ruiv3_send_cb);

	SEGGER_RTT_printf(0, "[INFO] LoRaWAN HAL initialized (band=%d, class=B)\r\n", OTAA_BAND);
}

void app_hal_lorawan_setup(void)
{
	/* ADR/rety/cfm 在入网后设置 (避免干扰入网过程) */
	api.lorawan.adr.set(true);
	api.lorawan.rety.set(1);
	api.lorawan.cfm.set(1);
	api.lorawan.timereq.set(1);
	/* 回调已在 app_hal_lorawan_init() 中注册, 此处不再重复 */
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

bool app_hal_is_beacon_locked(void) { return rx_beacon(); }

/* ── Beacon 状态机: 非阻塞, 由主循环每 10s 调用 ── */
static bool bcn_mc_done = false;  /* 多播已注册标志 */

void app_hal_beacon_start(void) {
	bcn_state = BCN_SEARCHING;
	bcn_phase_start_ms = millis();
	bcn_mc_done = false;
	SEGGER_RTT_printf(0, "[BCN] state=SEARCHING (timeout=%lus)\r\n",
		(unsigned long)(BCN_SEARCH_TIMEOUT_MS / 1000));
}

/* 进入 LOCKED 时调用 — 等待 Class B COMPLETED 后注册多播 */
static void bcn_try_multicast(void) {
	if (bcn_mc_done) return;

	int32_t st = service_lora_get_class_b_state();
	if (st == 4 /* COMPLETED */) {
		SEGGER_RTT_printf(0, "[BCN] Class B COMPLETED, setting up multicast\r\n");
		app_hal_setup_multicast();
		bcn_mc_done = true;
	}
	/* 若仍为 S2, 等待下次 tick 重试 (PingSlotInfo 交换完成后自动触发) */
}

void app_hal_beacon_tick(void) {
	uint32_t now = millis();
	bool has_bcn = rx_beacon();
	int32_t lora_st = service_lora_get_class_b_state();

	switch (bcn_state) {

	case BCN_IDLE:
		break;

	case BCN_SEARCHING:
		if (has_bcn) {
			bcn_state = BCN_LOCKED;
			SEGGER_RTT_printf(0, "[BCN] Beacon locked! btime=%lu\r\n",
				(unsigned long)api.lorawan.btime.get());
			app_hal_dump_classb_status();
			bcn_try_multicast();
		} else if (lora_st == 3 || now - bcn_phase_start_ms > BCN_SEARCH_TIMEOUT_MS) {
			/* RUI3 内部 S3_BeaconFailed 或超时 → 立即回退, 不等满 130s */
			bcn_state = BCN_FALLBACK;
			bcn_next_retry_ms = now + BCN_RETRY_INTERVAL_MS;
			SEGGER_RTT_printf(0, "[BCN] No beacon after %lus (lora_st=%ld), fallback to Class A\r\n",
				(unsigned long)((now - bcn_phase_start_ms) / 1000), lora_st);
		}
		break;

	case BCN_LOCKED:
		/* 每 tick 检查 Class B 是否达到 COMPLETED, 然后注册多播 */
		bcn_try_multicast();
		break;

	case BCN_FALLBACK:
		if (has_bcn) {
			bcn_state = BCN_LOCKED;
			bcn_mc_done = false;
			SEGGER_RTT_printf(0, "[BCN] Beacon recovered in fallback! btime=%lu\r\n",
				(unsigned long)api.lorawan.btime.get());
			bcn_try_multicast();
		} else if (now - bcn_next_retry_ms < 0x80000000UL /* now >= retry_ms */) {
			bcn_state = BCN_RETRY;
			bcn_phase_start_ms = now;
			api.lorawan.timereq.set(1);
			SEGGER_RTT_printf(0, "[BCN] state=RETRY (retry interval elapsed)\r\n");
		}
		break;

	case BCN_RETRY:
		if (has_bcn) {
			bcn_state = BCN_LOCKED;
			bcn_mc_done = false;
			SEGGER_RTT_printf(0, "[BCN] Beacon locked on retry! btime=%lu\r\n",
				(unsigned long)api.lorawan.btime.get());
			bcn_try_multicast();
		} else if (lora_st == 3 || now - bcn_phase_start_ms > BCN_SEARCH_TIMEOUT_MS) {
			/* RUI3 内部 S3_BeaconFailed 或超时 → 立即回退 */
			bcn_state = BCN_FALLBACK;
			bcn_next_retry_ms = now + BCN_RETRY_INTERVAL_MS;
			SEGGER_RTT_printf(0, "[BCN] Retry failed (lora_st=%ld), fallback to Class A\r\n", lora_st);
		}
		break;
	}
}

int app_hal_get_beacon_state(void) { return (int)bcn_state; }

/* ── Class B 状态诊断 ── */
void app_hal_dump_classb_status(void) {
	uint8_t dev_class = api.lorawan.deviceClass.get();
	uint8_t pgslot    = api.lorawan.pgslot.get();
	uint32_t bfreq_hz = (uint32_t)api.lorawan.bfreq.get();
	uint32_t btime    = api.lorawan.btime.get();
	int32_t  cb_state = service_lora_get_class_b_state();

	SEGGER_RTT_printf(0,
		"[CLSB] class=%d(%s) bcn=%d(%s) lora_st=%ld(%s) btime=%lu bfreq=%lu pgslot=%d\r\n",
		dev_class,
		dev_class == 0 ? "A" : dev_class == 1 ? "B" : dev_class == 2 ? "C" : "?",
		(int)bcn_state, bcn_state_name((int)bcn_state),
		cb_state, cls_b_state_name(cb_state),
		(unsigned long)btime,
		(unsigned long)bfreq_hz,
		pgslot);

	if (btime > 0) {
		beacon_bgw_t bgw = api.lorawan.bgw.get();
		/* SEGGER_RTT_printf 不支持 %%f, 用整数打印经纬度 (原始值 * 10000) */
		int32_t lat_raw = (int32_t)((double)bgw.latitude  * 90.0  / 8388607.0 * 10000.0);
		int32_t lon_raw = (int32_t)((double)bgw.longitude * 180.0 / 8388607.0 * 10000.0);
		SEGGER_RTT_printf(0,
			"[CLSB] GW: NetID=0x%06lX GWID=%lu GPS=%lu Lat=%ld.%04ld Lon=%ld.%04ld\r\n",
			(unsigned long)bgw.net_ID,
			(unsigned long)bgw.gateway_ID,
			(unsigned long)bgw.GPS_coordinate,
			(long)(lat_raw / 10000), (unsigned long)(lat_raw > 0 ? lat_raw % 10000 : (-lat_raw) % 10000),
			(long)(lon_raw / 10000), (unsigned long)(lon_raw > 0 ? lon_raw % 10000 : (-lon_raw) % 10000));
	}
}

/* ── 发送 (beacon lock 未就绪时阻塞上行, tx_busy 防 MAC 冲突) ── */
bool app_hal_send(uint8_t fport, const uint8_t *data, uint8_t len, bool confirmed) {
	if (!api.lorawan.njs.get()) {
		SEGGER_RTT_printf(0, "[WARN] TX blocked: not joined\r\n");
		return false;
	}
	if (tx_busy) {
		return false;  /* 上一个 TX+RX 周期未完成, 不调用 api.lorawan.send */
	}

	/* 非阻塞发送 — 不 delay 重试, 避免阻塞 actuatorThread 导致 LED/蜂鸣器冻结 */
	if (api.lorawan.send(len, (uint8_t *)data, fport, confirmed, 3)) {
		tx_busy = true;  /* TX 已提交, 等待 send_cb 清除 */
		return true;
	}
	SEGGER_RTT_printf(0, "[LORA] send FAIL fport=%d len=%d (radio busy)\r\n", fport, len);
	return false;
}

/* ── 多播组注册 (beacon lock 后调用) ──
 * 4 组 Class B 多播, 对应 Code Red/Blue/Yellow/Green.
 * 地址和密钥来自 board.h — 部署前替换为 ChirpStack 正式密钥.
 * 设备通过 match_multicast() (proto_handler.cpp) 决定是否响应.
 */
void app_hal_setup_multicast(void) {
	if (!rx_beacon()) {
		SEGGER_RTT_printf(0, "[INFO] Multicast skipped (no beacon)\r\n");
		return;
	}

	struct {
		uint32_t addr;
		uint8_t  nwkskey[16];
		uint8_t  appskey[16];
	} groups[] = {
		{ MC_RED_ADDR,    MC_RED_NWKSKEY,    MC_RED_APPSKEY },
		{ MC_BLUE_ADDR,   MC_BLUE_NWKSKEY,   MC_BLUE_APPSKEY },
		{ MC_YELLOW_ADDR, MC_YELLOW_NWKSKEY, MC_YELLOW_APPSKEY },
		{ MC_GREEN_ADDR,  MC_GREEN_NWKSKEY,  MC_GREEN_APPSKEY },
	};
	const int num_groups = sizeof(groups) / sizeof(groups[0]);

	/* 先用 lstmulc 遍历已存在的多播组, 打印并标记哪些地址已注册 */
	bool already_exists[4] = {false, false, false, false};
	SEGGER_RTT_printf(0, "[MC] Existing multicast groups:\r\n");
	{
		RAK_LORA_McSession existing;
		memset(&existing, 0, sizeof(existing));
		int cnt = 0;
		while (api.lorawan.lstmulc(&existing)) {
			SEGGER_RTT_printf(0, "[MC]   slot=%d cls=%d addr=0x%08X freq=%lu dr=%d\r\n",
				existing.McGroupID, existing.McDevclass, existing.McAddress,
				(unsigned long)existing.McFrequency, existing.McDatarate);
			for (int j = 0; j < num_groups; j++) {
				if (existing.McAddress == groups[j].addr)
					already_exists[j] = true;
			}
			cnt++;
		}
		if (cnt == 0) SEGGER_RTT_printf(0, "[MC]   (none)\r\n");
	}

	/* 只注册不存在的组 */
	int ok = 0;
	for (int i = 0; i < num_groups; i++) {
		if (already_exists[i]) {
			SEGGER_RTT_printf(0, "[INFO] MC group %d SKIP (already exists): addr=0x%08X\r\n",
				i, groups[i].addr);
			ok++;
			continue;
		}

		RAK_LORA_McSession session = {
			.McDevclass    = 1,              /* Class B */
			.McAddress     = groups[i].addr,
			.McFrequency   = MC_FREQ_HZ,     /* 923.3 MHz */
			.McDatarate    = MC_DATARATE,    /* DR8 = SF12/500kHz */
			.McPeriodicity = MC_PERIODICITY, /* 2^2 = 4s */
			.McGroupID     = (int8_t)i,      /* 0=Red,1=Blue,2=Yellow,3=Green */
			.entry         = 0,
		};
		memcpy(session.McAppSKey, groups[i].appskey, 16);
		memcpy(session.McNwkSKey, groups[i].nwkskey, 16);

		SEGGER_RTT_printf(0, "[MC] grp=%d cls=%d addr=0x%08X freq=%lu dr=%d period=%d\r\n",
			i, session.McDevclass, session.McAddress,
			(unsigned long)session.McFrequency, session.McDatarate,
			session.McPeriodicity);

		if (api.lorawan.addmulc(session)) {
			SEGGER_RTT_printf(0, "[INFO] MC group %d OK: addr=0x%08X\r\n", i, groups[i].addr);
			ok++;
		} else {
			int32_t st = service_lora_get_class_b_state();
			SEGGER_RTT_printf(0, "[ERROR] MC group %d FAIL: addr=0x%08X (class_b_state=%ld/%s)\r\n",
				i, groups[i].addr, st, cls_b_state_name(st));
		}
	}

	SEGGER_RTT_printf(0, "[INFO] Multicast setup: %d/%d groups\r\n", ok, num_groups);
}
