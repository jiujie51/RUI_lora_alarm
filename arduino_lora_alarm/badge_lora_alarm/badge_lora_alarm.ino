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

#include "boards/hub/board.h"

/* ── 公共模块 ── */
#include "debug_macros.h"
#include "app/join_state.h"
#include "app/alarm_sm.h"
#include "app/actuator_mgr.h"
#include "app/power_mgr.h"
#include "app/app_hal.h"
#include "proto/proto_internal.h"
#include "config/config_store.h"
#include "drv/led_strip.h"
#include "drv/buzzer_pwm.h"
#include "ble/ble_hub_adv.h"

/* ── 常量 ── */
#define HEARTBEAT_INTERVAL_MS       (HEARTBEAT_INTERVAL_SEC * 1000UL)
#define POWER_REPORT_INTERVAL_MS    (POWER_REPORT_INTERVAL_SEC * 1000UL)
#define WDT_FEED_INTERVAL_MS        30000UL

/* ── 全局状态 ── */
static volatile bool g_heartbeat_pending = false;
static volatile bool g_power_report_pending = false;
static uint8_t  last_power_pct = 255;
static uint32_t last_wdt_feed_ms = 0;

/* ── Protothread 控制块 ── */
rt rtLora, rtActuator;

/* ── 入网状态 ↦ actuator ── */
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

	if (len > 0 && app_hal_send(FPORT_HUB_UP, buf, len, false)) {
		LOG_INFO("main", "Heartbeat sent (%d bytes)", len);
	}
}

static void send_power_report(void) {
	uint8_t pct = power_mgr_get_battery_pct();
	if (abs((int)pct - (int)last_power_pct) < POWER_REPORT_DELTA_PCT) return;

	uint8_t buf[64];
	int len = proto_build_power(buf, sizeof(buf), DEV_TYPE_HUB, pct);

	if (len > 0 && app_hal_send(FPORT_COMMON, buf, len, false)) {
		last_power_pct = pct;
		LOG_INFO("main", "Power report: %d%%", pct);
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

	/* 等待入网完成 (非阻塞) */
	for (;;) {
		app_hal_join_tick();
		if (app_hal_is_joined()) break;
		RT_SLEEP(rt, 1000);  /* 1s 间隔检查 */
	}

	/* 入网成功后启用 Class B + 多播 */
	LOG_INFO("main", "=== LoRaWAN Joined ===");
	api.lorawan.deviceClass.set(RAK_LORA_CLASS_B);
	/* app_hal_setup_multicast(); */  /* TODO: ChirpStack McGroup */

	/* 首次心跳 */
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
		RT_SLEEP(rt, 100);  /* 100ms 间隔, 兼顾响应与功耗 */
	}

	RT_END(rt);
}

/* ══════════════════════════════════════════════════════════
 * actuatorThread — 10ms: LED/蜂鸣器 tick (NCS actuator_thread 等价)
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
	Serial.begin(115200, RAK_CUSTOM_MODE);
	delay(2000);
	LOG_INFO("main", "=== LoRa Alarm Hub v1.0 (RUI3) ===");

	/* 1. 协议引擎 */
	proto_engine_init();

	/* 2. 告警状态机 */
	alarm_sm_init();

	/* 3. 执行器管理器 */
	actuator_mgr_init();

	/* 4. 硬件驱动 */
	led_strip_init();
	buzzer_pwm_init();

	/* 5. 配置存储 (Flash) */
	config_store_init();

	/* 6. 电源管理 */
	power_mgr_init();

	/* 7. 设备信息 */
	uint8_t hub_type = config_get_hub_type();
	const char *type_names[] = {"RoomHub", "DoorHub", "HallwayHub"};
	LOG_INFO("main", "Hub type: %s (group=0x%02X room=%d)",
		(hub_type < 3) ? type_names[hub_type] : "Unknown",
		config_get_group_id(), config_get_room_id());

	/* 8. BLE 广播 (入网前启动) */
	ble_hub_adv_start();

	/* 9. LoRaWAN 初始化 (凭证 + 回调) */
	app_hal_lorawan_init();
	app_hal_set_downlink_cb(on_lora_downlink);

	/* 10. 定时器 */
	api.system.timer.create(RAK_TIMER_0, heartbeat_timer_cb, RAK_TIMER_PERIODIC);
	api.system.timer.start(RAK_TIMER_0, HEARTBEAT_INTERVAL_MS, NULL);

	api.system.timer.create(RAK_TIMER_1, power_report_timer_cb, RAK_TIMER_PERIODIC);
	api.system.timer.start(RAK_TIMER_1, POWER_REPORT_INTERVAL_MS, NULL);

	/* 11. 初始化 Protothreads */
	RT_INIT(&rtLora);
	RT_INIT(&rtActuator);

	LOG_INFO("main", "All modules initialized, system running");
}

/* ══════════════════════════════════════════════════════════
 * loop()
 * ══════════════════════════════════════════════════════════ */
void loop() {
	RT_SCHEDULE(loraThread(&rtLora));
	RT_SCHEDULE(actuatorThread(&rtActuator));

	/* WDT feed (30s, 用 millis 而非定时器以避免回调中喂狗) */
	uint32_t now = millis();
	if (now - last_wdt_feed_ms >= WDT_FEED_INTERVAL_MS) {
		last_wdt_feed_ms = now;
		/* api.system.wdt.feed(); — 如果 RUI3 支持 WDT */
	}
}
