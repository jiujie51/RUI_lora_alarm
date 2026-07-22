/*
 * LoRa Alarm System — Hub Firmware (RUI3/Arduino)
 *
 * 硬件: RAK4630 (nRF52840 + SX1262)
 * 外设: WS2812 LED 灯带 (15颗) + 蜂鸣器 + BLE 广播
 *
 * Protothreads 模型:
 *   loraThread      — LoRaWAN 入网 + 心跳/电量 TX (flag 驱动)
 *   actuatorThread  — 10ms: LED/蜂鸣器 tick
 *
 * 参考:
 *   ncs_lora_alarm/main.c (架构)
 *   RUI3 Example/RAK_Thread/RAK_Thread.ino (Protothreads)
 *   RUI3 Example/LoRaWan_OTAA/LoRaWan_OTAA.ino (LoRaWAN)
 */

/* ── RUI3 版本符号 ── */
extern "C" {
const char *sw_version  = "1.0.0";
const char *api_version = "4.2.4";
const char *cli_version = "1.0.0";
const char *model_id    = "RAK4630";
const char *chip_id     = "nRF52840";
const char *build_time  = __TIME__;
const char *build_date  = __DATE__;
const char *repo_info   = "lora-alarm-hub";
}

#include "src/boards/hub/board.h"

/* ── 公共模块 ── */
#include "nrf_log.h"

/* 调试: SEGGER_RTT 直接输出 (绕过 sdk_config.h 兼容问题)
 * RUI3 已初始化 RTT (NRF_LOG_BACKEND_RTT), Channel 0 即用
 * SEGGER_RTT_printf 在 RUI3 core 中已链接, 只需 extern 声明 */
extern "C" {
int SEGGER_RTT_printf(unsigned BufferIndex, const char * sFormat, ...);
int SEGGER_RTT_WriteString(unsigned BufferIndex, const char * s);
}
#define RTT_PRINTF(...)  SEGGER_RTT_printf(0, __VA_ARGS__)

#include "src/app/join_state.h"
#include "src/app/alarm_sm.h"
#include "src/app/actuator_mgr.h"
#include "src/app/power_mgr.h"
#include "src/app/app_hal.h"
#include "src/proto/proto_internal.h"
#include "src/config/config_store.h"
#include "src/drv/led_strip.h"
#include "src/drv/buzzer_pwm.h"
#include "src/ble/ble_hub_adv.h"

/* ── 常量 ── */
#define HEARTBEAT_INTERVAL_MS       (HEARTBEAT_INTERVAL_SEC * 1000UL)
#define POWER_REPORT_INTERVAL_MS    (POWER_REPORT_INTERVAL_SEC * 1000UL)
#define WDT_FEED_INTERVAL_MS        30000UL

/* ── 全局状态 ── */
static volatile bool g_heartbeat_pending = false;
static volatile bool g_power_report_pending = false;
static uint8_t  last_power_pct = 255;
static unsigned long last_wdt_feed_ms = 0;

/* ── Protothread 控制块 ── */
rt rtLora, rtActuator;

/* ── 入网状态 -> actuator ── */
int get_join_state(void) {
	return app_hal_get_join_state();
}

/* ── LoRaWAN 下行回调 ── */
static void on_lora_downlink(uint8_t port, const uint8_t *data, uint8_t len) {
	for (uint8_t i = 0; i < len; i++) {
		if (proto_parser_feed(data[i]) == 1) {
			struct proto_frame frame;
			if (proto_parser_get_frame(&frame) == 0) {
				proto_handle_frame(&frame);
			}
		}
	}
}

/* ── 上行函数 ── */
static void send_heartbeat(void) {
	uint8_t buf[64];
	int len = proto_build_heartbeat(buf, sizeof(buf),
		DEV_TYPE_HUB, config_get_group_id());
	if (len <= 0) {
		NRF_LOG_WARNING("Heartbeat build failed");
		return;
	}
	if (app_hal_send(FPORT_HUB_UP, buf, len, false)) {
		NRF_LOG_INFO("Heartbeat sent (%d bytes)", len);
	} else {
		NRF_LOG_WARNING("Heartbeat send failed! joined=%d len=%d",
			app_hal_is_joined(), len);
	}
}

static void send_power_report(void) {
	uint8_t pct = power_mgr_get_battery_pct();
	if (abs((int)pct - (int)last_power_pct) < POWER_REPORT_DELTA_PCT) return;

	uint8_t buf[64];
	int len = proto_build_power(buf, sizeof(buf), DEV_TYPE_HUB, pct);
	if (len <= 0) {
		NRF_LOG_WARNING("Power report build failed");
		return;
	}
	if (app_hal_send(FPORT_COMMON, buf, len, false)) {
		last_power_pct = pct;
		NRF_LOG_INFO("Power report: %d%% (%d bytes)", pct, len);
	} else {
		NRF_LOG_WARNING("Power report send failed! joined=%d",
			app_hal_is_joined());
	}
}

/* ── 定时器回调 (事件队列上下文, 非 ISR) ── */
static void heartbeat_timer_cb(void *) {
	if (app_hal_is_joined()) {
		g_heartbeat_pending = true;
	}
}

static void power_report_timer_cb(void *) {
	if (app_hal_is_joined()) {
		g_power_report_pending = true;
	}
}

/* ══════════════════════════════════════════════════════════
 * loraThread — LoRaWAN 入网 + 心跳/电量 TX (flag 驱动)
 * ══════════════════════════════════════════════════════════ */
int loraThread(struct rt *rt) {
	RT_BEGIN(rt);

	/* 等待入网完成 */
	for (;;) {
		app_hal_join_tick();
		if (app_hal_is_joined()) break;
		RT_SLEEP(rt, 1000);
	}

	NRF_LOG_INFO( "=== LoRaWAN Joined ===");
	//api.lorawan.deviceClass.set(RAK_LORA_CLASS_B);

	/* LoRaWAN 入网后 SoftDevice 可能挂起 BLE 广播, 重新拉起 */
	SEGGER_RTT_printf(0, "BLE: re-start after LoRaWAN join\n");
	ble_hub_adv_start();

	/* 等待 Class B beacon lock (US915 信标周期 128s, 超时 130s) */
	NRF_LOG_INFO("Waiting for Class B beacon lock...");
	for (int i = 0; i < 130; i++) {
		if (app_hal_is_beacon_locked()) break;
		RT_SLEEP(rt, 1000);
	}
	NRF_LOG_INFO("Beacon lock: %d", app_hal_is_beacon_locked());
	send_heartbeat();

	/* 主循环: 消费 TX flag */
	for (;;) {
		RT_YIELD(rt);

		if (g_heartbeat_pending) {
			g_heartbeat_pending = false;
			send_heartbeat();
		}
		if (g_power_report_pending) {
			g_power_report_pending = false;
			power_mgr_update();
			send_power_report();
		}
		RT_SLEEP(rt, 100);
	}

	RT_END(rt);
}

/* ══════════════════════════════════════════════════════════
 * actuatorThread — 10ms: LED/蜂鸣器 tick
 * ══════════════════════════════════════════════════════════ */
int actuatorThread(struct rt *rt) {
	RT_BEGIN(rt);
	for (;;) {
		actuator_mgr_tick();
		RT_SLEEP(rt, 10);
	}
	RT_END(rt);
}

/* ══════════════════════════════════════════════════════════
 * setup()
 * ══════════════════════════════════════════════════════════ */
void setup() {
	/*
	 * 初始化顺序很重要:
	 * 1. NFC 必须先禁用, 否则 P0.09 (蜂鸣器) 不可用
	 * 2. LoRaWAN 先于 BLE, 避免 radio 共享冲突
	 * 3. NeoPixel 先于 BLE, show() 关中断约 450us
	 *
	 * 调试: SEGGER_RTT_printf 直接写 RTT, 不依赖 NRF_LOG
	 * PC 端: JLinkRTTViewer 或 nRF Connect → RTT Viewer
	 */
	SEGGER_RTT_printf(0, "=== STEP 0: Boot ===\n");

	/* 0. 释放 NFC 引脚 (P0.09 蜂鸣器 / P0.10)
	 *    nRF52840 默认启用 NFC, 必须手动禁用以作为 GPIO */
	NRF_NFCT->TASKS_DISABLE = 1;
	SEGGER_RTT_printf(0, "NFC disabled — P0.09/P0.10 now GPIO\n");

	/* 1. LoRaWAN 初始化 (首次启动会 reboot) */
	SEGGER_RTT_printf(0, "=== STEP 1: LoRaWAN init (may reboot) ===\n");
	app_hal_lorawan_init();
	app_hal_set_downlink_cb(on_lora_downlink);
	SEGGER_RTT_printf(0, "=== STEP 1 done (no reboot) ===\n");

	/* 2. 协议引擎 + 告警 + 执行器 */
	SEGGER_RTT_printf(0, "=== STEP 2: proto/alarm/actuator init ===\n");
	proto_engine_init();
	alarm_sm_init();
	actuator_mgr_init();

	/* 3. Flash 配置 */
	SEGGER_RTT_printf(0, "=== STEP 3: config_store_init ===\n");
	config_store_init();

	/* 4. 电源管理 */
	SEGGER_RTT_printf(0, "=== STEP 4: power_mgr_init ===\n");
	power_mgr_init();

	/* 5. 硬件驱动 */
	SEGGER_RTT_printf(0, "=== STEP 5: led_strip_init ===\n");
	led_strip_init();  /* PWM+DMA mode, no SoftDevice conflict */
	SEGGER_RTT_printf(0, "=== STEP 6: buzzer_pwm_init ===\n");
	buzzer_pwm_init();

	/* 6. 设备信息 */
	SEGGER_RTT_printf(0, "Hub type=%d group=0x%02X room=%d\n",
		config_get_hub_type(), config_get_group_id(), config_get_room_id());

	/* 7. BLE 广播 */
	SEGGER_RTT_printf(0, "=== STEP 7: ble_hub_adv_start ===\n");
	ble_hub_adv_start();

	/* 8. 定时器 */
	SEGGER_RTT_printf(0, "=== STEP 8: Timers ===\n");
	api.system.timer.create(RAK_TIMER_0, heartbeat_timer_cb, RAK_TIMER_PERIODIC);
	api.system.timer.start(RAK_TIMER_0, HEARTBEAT_INTERVAL_MS, NULL);

	api.system.timer.create(RAK_TIMER_1, power_report_timer_cb, RAK_TIMER_PERIODIC);
	api.system.timer.start(RAK_TIMER_1, POWER_REPORT_INTERVAL_MS, NULL);

	/* 9. Protothreads */
	RT_INIT(&rtLora);
	RT_INIT(&rtActuator);

	SEGGER_RTT_printf(0, "=== STEP 9: ALL DONE ===\n");
}

/* ══════════════════════════════════════════════════════════
 * loop()
 * ══════════════════════════════════════════════════════════ */
void loop() {
	RT_SCHEDULE(loraThread(&rtLora));
	RT_SCHEDULE(actuatorThread(&rtActuator));

	/* WDT feed (30s) */
	unsigned long now = millis();
	if (now - last_wdt_feed_ms >= WDT_FEED_INTERVAL_MS) {
		last_wdt_feed_ms = now;
	}

	/* 心跳日志 (10s) — 确认系统活跃 */
	static unsigned long last_beacon_log = 0;
	if (now - last_beacon_log > 10000) {
		last_beacon_log = now;
		SEGGER_RTT_printf(0, "Heartbeat: joined=%d alarm=%d batt=%d%%\n",
			app_hal_is_joined(),
			alarm_sm_current_priority(),
			power_mgr_get_battery_pct());
	}
}
