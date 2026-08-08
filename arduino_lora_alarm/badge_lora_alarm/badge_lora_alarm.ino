/*
 * @Author: jiefengzhu focus_feng@163.com
 * @Date: 2026-07-29 20:12:53
 * @LastEditors: jiefengzhu focus_feng@163.com
 * @LastEditTime: 2026-08-03 23:54:40
 * @FilePath: \RUI_lora_alarm\arduino_lora_alarm\badge_lora_alarm\badge_lora_alarm.ino
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
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
#include "src/config/device_identity.h"
#include "src/fuota/fuota_handler.h"
#include "src/drv/led_strip.h"
#include "src/drv/buzzer_pwm.h"
#include "src/drv/gps_drv.h"
#if OLED_ENABLE
#include "src/drv/oled_drv.h"
#endif
#include "src/ui/badge_ui.h"
#include "src/ble/ble_badge_scan.h"

/* ── 常量 ── */
#define PERIODIC_TX_INTERVAL_MS   (HEARTBEAT_INTERVAL_SEC * 1000UL)
/* ── 硬件自检 ──
 * 1 = 上电后依次测试 LED/OLED/马达/蜂鸣器/按键/GPS
 * 0 = 跳过
 */
#define HW_SELF_TEST  1

/* ── 全局状态 ── */
static volatile bool g_periodic_tx_pending = false;
static uint8_t  last_power_pct = 255;

#if OLED_ENABLE
/* OLED: SSD1306 128x64 @ I2C 0x3C, NRF_TWIM1 (P0.29/SDA P0.30/SCL) */
#endif

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
	// ble_scan_reinit();
	app_hal_lorawan_setup();
	send_heartbeat();

	/* 启动 Class B beacon 搜索 (非阻塞, 由主循环 app_hal_beacon_tick 驱动)
	 * 上行消息(心跳/电量)不等待 beacon — 立即开始正常发送.
	 * Beacon 搜索超时后自动回退 Class A, 并定期重试. */
	app_hal_beacon_start();


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
				app_hal_beacon_tick();
				app_hal_dump_classb_status();
				SEGGER_RTT_printf(0, "[ALIVE] joined=%d alarm=%d batt=%d%% chg=%d scan=%d gps=%d\n",
					app_hal_is_joined(),
					alarm_sm_current_priority(),
					power_mgr_get_battery_pct(),
					power_mgr_is_charging(),
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
	delay(2000);
	SEGGER_RTT_printf(0, "=== STEP 0: Boot (Badge) ===\n");
	SEGGER_RTT_printf(0, "[INFO] === LoRa Alarm Badge v1.0 (RUI3) ===\n");

	/* 0. FUOTA: 检查并执行待处理的固件升级
	 *    必须在所有外设初始化之前 (成功时不返回) */
	SEGGER_RTT_printf(0, "=== STEP 0a: FUOTA check ===\n");
	fuota_apply_if_pending();
	SEGGER_RTT_printf(0, "=== STEP 0a: No pending FUOTA ===\n");

	/* 0b. 释放 NFC 引脚 (P0.09 蜂鸣器 / P0.10)
	 *     nRF52840 默认启用 NFC, 必须手动禁用以作为 GPIO */
	NRF_NFCT->TASKS_DISABLE = 1;
	SEGGER_RTT_printf(0, "NFC disabled — P0.09/P0.10 now GPIO\n");

	/* 1. 设备身份: BLE MAC + LoRaWAN 凭证 (flash 0xB4000, 生产时预烧录) */
	SEGGER_RTT_printf(0, "=== STEP 1: device_identity_init ===\n");
	device_identity_init();

	/* 2. BLE 协议栈初始化 (必须在 LoRaWAN 之前, 否则冲突重启) */
	SEGGER_RTT_printf(0, "=== STEP 2: BLE init ===\n");
	ble_scan_init();

	/* 3. LoRaWAN 初始化 */
	SEGGER_RTT_printf(0, "=== STEP 3: LoRaWAN init (may reboot) ===\n");
	app_hal_lorawan_init();
	app_hal_set_downlink_cb(on_lora_downlink);
	fuota_init();  /* 注册 FUOTA 进度/完成回调 */
	SEGGER_RTT_printf(0, "=== STEP 3 done ===\n");

	/* 4. 协议引擎 + 告警 + 执行器 */
	SEGGER_RTT_printf(0, "=== STEP 4: proto/alarm/actuator init ===\n");
	proto_engine_init();
	/* 5. Flash 配置 (必须在 actuator_mgr_init 之前, alarm config 从 flash 恢复) */
	SEGGER_RTT_printf(0, "=== STEP 5: config_store_init ===\n");
	config_store_init();

	alarm_sm_init();
	actuator_mgr_init();

	/* 6. 电源管理 */
	SEGGER_RTT_printf(0, "=== STEP 6: power_mgr_init ===\n");
	power_mgr_init();

	/* 7. 硬件驱动 */
	SEGGER_RTT_printf(0, "=== STEP 7: led_strip_init ===\n");
	led_strip_init();
	led_strip_set_all({0, 255, 0});//for test oled is init ok
	SEGGER_RTT_printf(0, "=== STEP 8: buzzer_pwm_init ===\n");
	buzzer_pwm_init();

#if GPS_ENABLE
		SEGGER_RTT_printf(0, "=== STEP 6b: gps_drv_init ===\n");
		gps_drv_init();
#endif

#if OLED_ENABLE
		SEGGER_RTT_printf(0, "=== STEP 6c: oled_init ===\n");
		pinMode(OLED_PWR_PIN, OUTPUT);
		digitalWrite(OLED_PWR_PIN, HIGH);
		delay(100);  /* SSD1306 上电稳定 */
		pinMode(OLED_RES_PIN, OUTPUT);
		digitalWrite(OLED_RES_PIN, LOW); delay(50);
		digitalWrite(OLED_RES_PIN, HIGH); delay(100);
		oled_init();
		SEGGER_RTT_printf(0, "  OLED initialized (SSD1306 128x64)\n");
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

		/* LED: PWM 输出 (电压钳位 0.65V-1.45V) */
		SEGGER_RTT_printf(0, "  LED PWM test (R->G->B):\n");
		led_strip_off();
		// SEGGER_RTT_printf(0, "    OFF\n");
		// delay(500);
		// SEGGER_RTT_printf(0, "    RED...");
		// led_strip_set_all({255, 0, 0}); delay(300);
		// led_strip_off();
		// SEGGER_RTT_printf(0, "OK\n");
		// SEGGER_RTT_printf(0, "    GREEN...");
		// led_strip_set_all({0, 255, 0}); delay(300);
		// led_strip_off();
		// SEGGER_RTT_printf(0, "OK\n");
		// SEGGER_RTT_printf(0, "    BLUE...");
		// led_strip_set_all({0, 0, 255}); delay(300);
		// led_strip_off();
		// SEGGER_RTT_printf(0, "OK\n");
		// SEGGER_RTT_printf(0, "    WHITE...");
		led_strip_set_all({255, 255, 255}); delay(500);
		led_strip_off();
		SEGGER_RTT_printf(0, "OK\n");
		SEGGER_RTT_printf(0, "  LED PWM test done\n");

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

#if GPS_ENABLE
		/* GPS: 读取 NMEA 报文, RTT 输出 (超时 5s, 最多 10 行) */
		{
			SEGGER_RTT_printf(0, "  GPS: reading NMEA...\n");
			uint32_t gps_start = millis();
			uint8_t nmea_lines = 0;
			char nmea_buf[128];
			uint8_t nmea_pos = 0;
			while (millis() - gps_start < 5000 && nmea_lines < 10) {
				while (GPS_UART.available() > 0) {
					char c = (char)GPS_UART.read();
					if (c == '\n') {
						if (nmea_pos > 0 && nmea_buf[nmea_pos - 1] == '\r')
							nmea_buf[nmea_pos - 1] = '\0';
						else nmea_buf[nmea_pos] = '\0';
						if (nmea_buf[0] == '$')
							SEGGER_RTT_printf(0, "  GPS[%d]: %s\n", nmea_lines, nmea_buf);
						nmea_lines++;
						nmea_pos = 0;
					} else if (nmea_pos < (int)sizeof(nmea_buf) - 1) {
						nmea_buf[nmea_pos++] = c;
					}
				}
				delay(50);
			}
			
			if (nmea_lines == 0)
			{
				SEGGER_RTT_printf(0, "  GPS: no NMEA data (check antenna/connection)\n");
			}
			else
			{
				buzzer_pwm_set(BUZZER_ON, 10, 0, 0); delay(150);
				buzzer_pwm_off();
			}
			
			SEGGER_RTT_printf(0, "  GPS test done (%d lines)\n", nmea_lines);
		}
#endif

#if OLED_ENABLE
		/* OLED: 全屏亮→灭→文字, 确认硬件通路 */
		// SEGGER_RTT_printf(0, "  OLED: test pattern...\n");
		// /* 全白 */
		// 	oled_fill_screen(0xFF); delay(300);
		// 	/* 棋盘格 */
		// 	oled_fill_screen(0x55); delay(300);
		// 	/* 文字 */
			oled_clear();
		// 	oled_draw_string(0, 0, "Badge v1.0");
		// 	oled_draw_string(0, 2, "OLED OK!");
		// SEGGER_RTT_printf(0, "  OLED: OK\n");
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
