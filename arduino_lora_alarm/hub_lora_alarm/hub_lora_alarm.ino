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

/* 调试: SEGGER_RTT 统一输出
 * SEGGER_RTT_printf 在 RUI3 core 中已链接, 只需 extern 声明 */
extern "C" {
int SEGGER_RTT_printf(unsigned BufferIndex, const char * sFormat, ...);
}

#include "src/config/device_identity.h"
#include "src/fuota/fuota_handler.h"
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
/* ── 全局状态 ── */
static volatile bool g_heartbeat_pending = false;
static volatile bool g_power_report_pending = false;
static uint8_t  last_power_pct = 255;

/* ── Protothread 控制块 ── */
rt rtLora, rtActuator;

/* ── 入网状态 -> actuator ── */
int get_join_state(void) {
	return app_hal_get_join_state();
}

/* ── LoRaWAN 下行回调 ── */
static void on_lora_downlink(uint8_t port, const uint8_t *data, uint8_t len) {
	proto_parser_parse(data, len);
}

/* ── 上行函数 (返回 true=发送成功) ── */
static bool send_heartbeat(void) {
	uint8_t buf[64];
	int len = proto_build_heartbeat(buf, sizeof(buf),
		DEV_TYPE_HUB, config_get_group_id());
	if (len <= 0) {
		SEGGER_RTT_printf(0, "[WARN] Heartbeat build failed\n");
		return false;
	}
	if (app_hal_send(FPORT_HUB_UP, buf, len, false)) {
		SEGGER_RTT_printf(0, "[INFO] Heartbeat sent (%d bytes)\n", len);
		return true;
	} else {
		SEGGER_RTT_printf(0, "[WARN] Heartbeat send failed! joined=%d len=%d\n",
			app_hal_is_joined(), len);
		return false;
	}
}

static bool send_power_report(void) {
	uint8_t pct = power_mgr_get_battery_pct();
	if (abs((int)pct - (int)last_power_pct) < POWER_REPORT_DELTA_PCT) return false;

	uint8_t buf[64];
	int len = proto_build_power(buf, sizeof(buf), DEV_TYPE_HUB, pct);
	if (len <= 0) {
		SEGGER_RTT_printf(0, "[WARN] Power report build failed\n");
		return false;
	}
	if (app_hal_send(FPORT_HUB_UP, buf, len, false)) {
		last_power_pct = pct;
		SEGGER_RTT_printf(0, "[INFO] Power report: %d%% (%d bytes)\n", pct, len);
		return true;
	} else {
		SEGGER_RTT_printf(0, "[WARN] Power report send failed! joined=%d\n",
			app_hal_is_joined());
		return false;
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

	SEGGER_RTT_printf(0, "[INFO] === LoRaWAN Joined ===\r\n");

	/* LoRaWAN 入网后 SoftDevice 可能挂起 BLE 广播, 重新拉起 */
	SEGGER_RTT_printf(0, "BLE: re-start after LoRaWAN join\r\n");
	ble_hub_adv_start();

	app_hal_lorawan_setup();
	send_heartbeat();

	/* 启动 Class B beacon 搜索 (非阻塞, 由主循环 app_hal_beacon_tick 驱动)
	 * 上行消息(心跳/电量)不等待 beacon — 立即开始正常发送.
	 * Beacon 搜索超时后自动回退 Class A, 并定期重试. */
	app_hal_beacon_start();

	/* 主循环: 消费 TX flag */
	for (;;) {
		RT_YIELD(rt);

		if (g_heartbeat_pending) {
			if (send_heartbeat()) {
				g_heartbeat_pending = false;
			}
		} else if (g_power_report_pending) {
			power_mgr_update();
			if (send_power_report()) {
				g_power_report_pending = false;
			}
		}

		/* 心跳日志 (10s) — 确认系统活跃, 驱动 Beacon 状态机 */
		{
			static uint32_t last_log = 0;
			uint32_t now = millis();
			if (now - last_log > 10000) {
				last_log = now;
				app_hal_beacon_tick();
				app_hal_dump_classb_status();
				SEGGER_RTT_printf(0, "[ALIVE] joined=%d alarm=%d batt=%d%%\r\n",
					app_hal_is_joined(),
					alarm_sm_current_priority(),
					power_mgr_get_battery_pct());

				/* BLE 看门狗: 每 30s 重新拉起广播 */
				{
					static uint32_t last_ble_wd = 0;
					if (now - last_ble_wd > 30000) {
						last_ble_wd = now;
						SEGGER_RTT_printf(0, "[BLE] watchdog refresh\r\n");
						ble_hub_adv_start();
					}
				}
			}
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
 * LED/蜂鸣器硬件自检 (编译开关 LED_SELF_TEST)
 * ══════════════════════════════════════════════════════════ */
#if LED_SELF_TEST
static void led_strip_self_test(void)
{
	struct led_color red   = {255,   0,   0};
	struct led_color green = {  0, 255,   0};
	struct led_color blue  = {  0,   0, 255};
	struct led_color white = { 64,  64,  64};

	/* 使能 LED 供电 */
	pinMode(LED_PWR_PIN, OUTPUT);
	digitalWrite(LED_PWR_PIN, HIGH);
	delay(10);

	SEGGER_RTT_printf(0, "[SELFTEST] Red\r\n");
	led_strip_set_all(red);
	delay(300);

	SEGGER_RTT_printf(0, "[SELFTEST] Green\r\n");
	led_strip_set_all(green);
	delay(300);

	SEGGER_RTT_printf(0, "[SELFTEST] Blue\r\n");
	led_strip_set_all(blue);
	delay(300);

	SEGGER_RTT_printf(0, "[SELFTEST] White (low brightness)\r\n");
	led_strip_set_all(white);
	delay(300);

	SEGGER_RTT_printf(0, "[SELFTEST] Buzzer\r\n");
	buzzer_pwm_set(BUZZER_ON, 5, 200, 0);
	delay(200);
	buzzer_pwm_off();

	SEGGER_RTT_printf(0, "[SELFTEST] Done — all off\r\n");
	led_strip_off();
}
#endif

/* ══════════════════════════════════════════════════════════
 * setup()
 * ══════════════════════════════════════════════════════════ */
void setup() {
	delay(2000);
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

	/* 0a. FUOTA: 检查并执行待处理的固件升级
	 *     必须在所有外设初始化之前 (成功时不返回) */
	SEGGER_RTT_printf(0, "=== STEP 0a: FUOTA check ===\n");
	fuota_apply_if_pending();
	SEGGER_RTT_printf(0, "=== STEP 0a: No pending FUOTA ===\n");

	/* 0b. 释放 NFC 引脚 (P0.09 蜂鸣器 / P0.10)
	 *     nRF52840 默认启用 NFC, 必须手动禁用以作为 GPIO */
	NRF_NFCT->TASKS_DISABLE = 1;
	SEGGER_RTT_printf(0, "NFC disabled — P0.09/P0.10 now GPIO\n");

	/* 0c. 设备身份: BLE MAC + LoRaWAN 凭证 (flash 0xB4000, 生产时预烧录) */
	SEGGER_RTT_printf(0, "=== STEP 0c: device_identity_init ===\n");
	device_identity_init();

	/* 1. LoRaWAN 初始化 (首次启动会 reboot) */
	SEGGER_RTT_printf(0, "=== STEP 1: LoRaWAN init (may reboot) ===\n");
	app_hal_lorawan_init();
	app_hal_set_downlink_cb(on_lora_downlink);
	fuota_init();  /* 注册 FUOTA 回调 */
	SEGGER_RTT_printf(0, "=== STEP 1 done (no reboot) ===\n");

	/* 2. Flash 配置 (必须在 proto_engine_init 之前) */
	SEGGER_RTT_printf(0, "=== STEP 2: config_store_init ===\n");
	config_store_init();

	/* 3. 协议引擎 */
	SEGGER_RTT_printf(0, "=== STEP 3: proto init ===\n");
	proto_engine_init();

	/* 4. 告警 + 执行器 */
	SEGGER_RTT_printf(0, "=== STEP 4: alarm/actuator init ===\n");
	alarm_sm_init();
	actuator_mgr_init();

	/* 4. 电源管理 */
	SEGGER_RTT_printf(0, "=== STEP 4: power_mgr_init ===\n");
	power_mgr_init();

	/* 5. 硬件驱动 */
	SEGGER_RTT_printf(0, "=== STEP 5: led_strip_init ===\n");
	led_strip_init();  /* PWM+DMA mode, no SoftDevice conflict */
	SEGGER_RTT_printf(0, "=== STEP 6: buzzer_pwm_init ===\n");
	buzzer_pwm_init();

#if LED_SELF_TEST
	/* 5a. LED/蜂鸣器硬件自检: 红→绿→蓝→白→蜂鸣器→灭
	 *     仅首次上电运行 (闪存无有效 identity 时), 已有 identity 则跳过 */
	if (!device_identity_is_from_flash()) {
		SEGGER_RTT_printf(0, "=== STEP 5a: LED/Buzzer self-test ===\n");
		led_strip_self_test();
	}
#endif

	/* 6. 设备信息 */
	SEGGER_RTT_printf(0, "Hub type=%d group=0x%02X room=%d\n",
		config_get_hub_type(), config_get_group_id(), config_get_room_id());

	/* 7. BLE 广播 */
	SEGGER_RTT_printf(0, "=== STEP 7: ble_hub_adv_start ===\n");
	ble_hub_adv_start();

	/* 7a. 首次上电: 将 BLE MAC (SoftDevice Random Static) 持久化到 identity flash
	 *     仅在 flash 中无有效 identity 时写入, 已有则跳过 */
	SEGGER_RTT_printf(0, "=== STEP 7a: device_identity_persist ===\n");
	{
		int ret = device_identity_persist();
		SEGGER_RTT_printf(0, "[SETUP] device_identity_persist() = %d\r\n", ret);
	}

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
}
