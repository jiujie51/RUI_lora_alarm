/*
 * LoRa Alarm System — Badge Firmware (RUI3/Arduino)
 *
 * 硬件: RAK4630 (nRF52840 + SX1262)
 * 外设: 4 按键(R/G/B/Y) + OLED SSD1306 + GPS + 蜂鸣器 + 振动马达 + RGB LED×3 + BLE 扫描
 *
 * Protothreads 模型:
 *   loraThread      — LoRaWAN 入网 + 心跳/电量 TX (flag 驱动)
 *   actuatorThread  — 10ms: LED/蜂鸣器 tick
 *
 * 参考:
 *   Hub firmware (hub_lora_alarm/hub_lora_alarm.ino)
 *   RUI3 Example/LoRaWan_OTAA/LoRaWan_OTAA.ino
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
const char *repo_info   = "lora-alarm-badge";
}

#include "src/boards/badge/board.h"

/* 调试: SEGGER_RTT 统一输出 (替代 NRF_LOG, 避免 LoRaWAN init 后 nrf_log 后端异常)
 * RUI3 已初始化 RTT (NRF_LOG_BACKEND_RTT), Channel 0 即用
 * SEGGER_RTT_printf 在 RUI3 core 中已链接, 只需 extern 声明 */
extern "C" {
int SEGGER_RTT_printf(unsigned BufferIndex, const char * sFormat, ...);
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
#include "src/drv/gps_drv.h"
#include "src/ui/badge_ui.h"
#include "src/ble/ble_badge_scan.h"

/* ── 常量 ── */
#define PERIODIC_TX_INTERVAL_MS   (HEARTBEAT_INTERVAL_SEC * 1000UL)
/* ── 硬件自检 ──
 * 1 = 上电后依次测试 LED/OLED/马达/蜂鸣器/按键
 * 0 = 跳过
 */
#define HW_SELF_TEST  1

/* ── 全局状态 ── */
static volatile bool g_periodic_tx_pending = false;
static uint8_t  last_power_pct = 255;

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
			static struct proto_frame frame;
			if (proto_parser_get_frame(&frame) == 0) {
				proto_handle_frame(&frame);
			}
		}
	}
}

/* ── 上行函数 ── */
static void send_heartbeat(void) {
	static uint8_t buf[64];
	int len = proto_build_heartbeat(buf, sizeof(buf),
		DEV_TYPE_BADGE, config_get_group_id());
	if (len <= 0) {
		SEGGER_RTT_printf(0, "[WARN] Heartbeat build failed\n");
		return;
	}
	if (app_hal_send(FPORT_BADGE_UP, buf, len, false)) {
		SEGGER_RTT_printf(0, "[INFO] Heartbeat sent (%d bytes)\n", len);
	} else {
		SEGGER_RTT_printf(0, "[WARN] Heartbeat send failed! joined=%d len=%d\n",
			app_hal_is_joined(), len);
	}
}

static void send_power_report(void) {
	uint8_t pct = power_mgr_get_battery_pct();
	if (abs((int)pct - (int)last_power_pct) < POWER_REPORT_DELTA_PCT) return;

	static uint8_t buf[64];
	int len = proto_build_power(buf, sizeof(buf), DEV_TYPE_BADGE, pct);
	if (len <= 0) {
		SEGGER_RTT_printf(0, "[WARN] Power report build failed\n");
		return;
	}
	if (app_hal_send(FPORT_COMMON, buf, len, false)) {
		last_power_pct = pct;
		SEGGER_RTT_printf(0, "[INFO] Power report: %d%%\n", pct);
	} else {
		SEGGER_RTT_printf(0, "[WARN] Power report send failed! joined=%d\n",
			app_hal_is_joined());
	}
}

/* ── 定时器回调 ── */
static void periodic_timer_cb(void *) {
	SEGGER_RTT_printf(0, "[TIMER] tick\n");
	if (app_hal_is_joined()) {
		g_periodic_tx_pending = true;
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

	SEGGER_RTT_printf(0, "[INFO] === LoRaWAN Joined ===\n");

	/* LoRaWAN 入网后重置 BLE 扫描数据, 但不重启扫描器 (避免 SoftDevice 断言) */
	SEGGER_RTT_printf(0, "BLE: re-init scanner after LoRaWAN join\n");
	ble_scan_reinit();

	/* 等待 Class B beacon lock (US915 信标周期 128s, 超时 130s) */
	SEGGER_RTT_printf(0, "[INFO] Waiting for Class B beacon lock...\n");
	for (int i = 0; i < 130; i++) {
		if (app_hal_is_beacon_locked()) break;
		RT_SLEEP(rt, 1000);
	}
	SEGGER_RTT_printf(0, "[INFO] Beacon lock: %d\n", app_hal_is_beacon_locked());

	/* 配置 Class B 多播组 (4 组, 用于下行告警广播) */
	SEGGER_RTT_printf(0, "Setting up multicast groups...\n");
	app_hal_setup_multicast();

	send_heartbeat();

	/* 主循环: 消费 TX flag */
	for (;;) {
		RT_YIELD(rt);

		if (g_periodic_tx_pending) {
			g_periodic_tx_pending = false;
			SEGGER_RTT_printf(0, "[TX] heartbeat+power start\n");
			send_heartbeat();
			power_mgr_update();
			send_power_report();
#if GPS_ENABLE
			gps_drv_check_timeout();
#endif
			SEGGER_RTT_printf(0, "[TX] heartbeat+power done\n");
		}

		/* 心跳日志 (10s) — 确认系统活跃 */
		{
			static uint32_t last_log = 0;
			uint32_t now = millis();
			if (now - last_log > 10000) {
				last_log = now;
				SEGGER_RTT_printf(0, "[ALIVE] joined=%d alarm=%d batt=%d%% scan=%d gps=%d\n",
					app_hal_is_joined(),
					alarm_sm_current_priority(),
					power_mgr_get_battery_pct(),
					ble_scan_active(),
#if GPS_ENABLE
					gps_drv_has_fix()
#else
					0
#endif
				);
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
			badge_ui_poll();
			ble_scan_process();
#if GPS_ENABLE
			gps_drv_poll();
#endif
			RT_SLEEP(rt, 10);
	}
	RT_END(rt);
}

/* ══════════════════════════════════════════════════════════
 * setup()
 * ══════════════════════════════════════════════════════════ */
void setup() {

	SEGGER_RTT_printf(0, "=== STEP 0: Boot (Badge) ===\n");
	SEGGER_RTT_printf(0, "[INFO] === LoRa Alarm Badge v1.0 (RUI3) ===\n");

	/* 0. 释放 NFC 引脚 (P0.09 蜂鸣器 / P0.10)
	 *    nRF52840 默认启用 NFC, 必须手动禁用以作为 GPIO */
	NRF_NFCT->TASKS_DISABLE = 1;
	SEGGER_RTT_printf(0, "NFC disabled — P0.09/P0.10 now GPIO\n");

	/* 1. BLE 协议栈初始化 (必须在 LoRaWAN 之前, 否则冲突重启) */
	SEGGER_RTT_printf(0, "=== STEP 1: BLE init ===\n");
	ble_scan_init();

	/* 2. LoRaWAN 初始化 */
	SEGGER_RTT_printf(0, "=== STEP 2: LoRaWAN init (may reboot) ===\n");
	app_hal_lorawan_init();
	app_hal_set_downlink_cb(on_lora_downlink);
	SEGGER_RTT_printf(0, "=== STEP 2 done ===\n");

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
	led_strip_init();
	SEGGER_RTT_printf(0, "=== STEP 6: buzzer_pwm_init ===\n");
	buzzer_pwm_init();

#if GPS_ENABLE
		SEGGER_RTT_printf(0, "=== STEP 6b: gps_drv_init ===\n");
		gps_drv_init();
#endif

	/* 7. Badge UI (按键 + 两段式确认) */
	SEGGER_RTT_printf(0, "=== STEP 7: badge_ui_init ===\n");
	badge_ui_init();

	/* 7. 设备信息 */
	SEGGER_RTT_printf(0, "Badge group=0x%02X room=%d\n",
		config_get_group_id(), config_get_room_id());

#if HW_SELF_TEST
	/* 6.5 硬件自检 */
	{
		SEGGER_RTT_printf(0, "=== STEP 6.5: HW Self-Test ===\n");

		/* LED: R→G→B 各 200ms (使用 udrv_pwm 直驱, 不干扰端口绑定) */
		SEGGER_RTT_printf(0, "  LED: R...");
		led_strip_set_all({128, 0, 0}); delay(200);
		SEGGER_RTT_printf(0, "G...");
		led_strip_set_all({0, 128, 0}); delay(200);
		SEGGER_RTT_printf(0, "B...");
		led_strip_set_all({0, 0, 128}); delay(200);
		led_strip_off();
		SEGGER_RTT_printf(0, "OK\n");

		/* 马达: 100ms 短震 */
		SEGGER_RTT_printf(0, "  Motor: pulse...");
		pinMode(MOTOR_PIN, OUTPUT);
		digitalWrite(MOTOR_PIN, HIGH); delay(100);
		digitalWrite(MOTOR_PIN, LOW);
		SEGGER_RTT_printf(0, "OK\n");

		/* 蜂鸣器 (TIMER4 驱动, 不占 PWM 池) */
		SEGGER_RTT_printf(0, "  Buzzer: beep...");
		buzzer_pwm_set(BUZZER_ON, 10, 0, 0); delay(150);
		buzzer_pwm_off();
		SEGGER_RTT_printf(0, "OK\n");

		/* 按键: 读取电平 (上拉, 按下=0, badge_ui_init 已配置引脚) */
		delay(5);
		int r = digitalRead(BUTTON_RED_PIN);
		int g = digitalRead(BUTTON_GREEN_PIN);
		int b = digitalRead(BUTTON_BLUE_PIN);
		int y = digitalRead(BUTTON_YELLOW_PIN);
		SEGGER_RTT_printf(0, "  Buttons: R=%d G=%d B=%d Y=%d (%s)\n",
			r, g, b, y,
			(!r||!g||!b||!y) ? "SOME PRESSED" : "all released");

#if OLED_ENABLE
		/* OLED: I2C 扫描 */
		SEGGER_RTT_printf(0, "  OLED: init...");
		pinMode(OLED_PWR_PIN, OUTPUT);
		digitalWrite(OLED_PWR_PIN, HIGH);
		pinMode(OLED_RES_PIN, OUTPUT);
		digitalWrite(OLED_RES_PIN, HIGH);
		delay(10);
		Wire.begin();
		Wire.beginTransmission(OLED_I2C_ADDR);
		if (Wire.endTransmission() == 0)
			SEGGER_RTT_printf(0, "found at 0x%02X\n", OLED_I2C_ADDR);
		else
			SEGGER_RTT_printf(0, "NOT FOUND\n");
#endif

		SEGGER_RTT_printf(0, "=== HW Self-Test DONE ===\n");
	}
#endif

	/* 7. 定时器 */
	SEGGER_RTT_printf(0, "=== STEP 7: Timers ===\n");
	api.system.timer.create(RAK_TIMER_0, periodic_timer_cb, RAK_TIMER_PERIODIC);
	api.system.timer.start(RAK_TIMER_0, PERIODIC_TX_INTERVAL_MS, NULL);

	/* 8. Protothreads */
	SEGGER_RTT_printf(0, "=== STEP 8: ALL DONE ===\n");
	RT_INIT(&rtLora);
	RT_INIT(&rtActuator);

	SEGGER_RTT_printf(0, "[INFO] All modules initialized, system running\n");
}

/* ══════════════════════════════════════════════════════════
 * loop()
 * ══════════════════════════════════════════════════════════ */
void loop() {
	RT_SCHEDULE(loraThread(&rtLora));
	RT_SCHEDULE(actuatorThread(&rtActuator));
}
