# Memory Safety Audit Report — 2026-07-10

## 审计范围

`firmware/src/` 全部源文件，重点检查数组越界、缓冲区溢出、空指针、枚举/定义不匹配。

---

## 🔴 Critical (1)

### 1. proto_builder.c:68 — 栈缓冲区溢出 ✅ 已修复

**位置:** `firmware/src/proto/proto_builder.c:68`

```c
uint8_t data[16];  // ❌ 只分配 16 字节

// 实际写入: button(1) + motion(1) + rssi(1) + mac(6) + lat(4) + lon(4) = 17 字节
*p++ = button;
*p++ = motion;
*p++ = (uint8_t)(rssi < 0 ? -rssi : rssi);
memcpy(p, hub_mac, 6); p += 6;
*p++ = (lat >> 24) & 0xFF;   // 4 bytes
*p++ = (lat >> 16) & 0xFF;
*p++ = (lat >> 8) & 0xFF;
*p++ = lat & 0xFF;
*p++ = (lon >> 24) & 0xFF;   // 4 bytes
*p++ = (lon >> 16) & 0xFF;
*p++ = (lon >> 8) & 0xFF;
*p++ = lon & 0xFF;           // ← 溢出点: data[16] 第17字节
```

**触发路径:** Badge 按键 → `ui_trigger_alert()` → `send_key_event_uplink()` → `proto_build_key_event()`  
**影响:** 每次按键触发告警都栈溢出 1 字节，破坏相邻栈变量，累积后导致 MPU FAULT (logging 线程崩溃)  
**修复:** `data[16]` → `data[18]`（已应用）

---

## 🟡 High (3)

### 2. proto_handler.c:147 — handle_led_control 越界读

**位置:** `firmware/src/proto/proto_handler.c:147-149`

```c
if (len < 7) return -EINVAL;                     // ❌ 应该是 len < 8
return actuator_led_override(data[0], data[1], data[2],
    data[3], (data[4]<<8)|data[5], (data[6]<<8)|data[7]);  // 读 data[7] 需要 8 字节
```

**格式:** `r(1)+g(1)+b(1)+mode(1)+on_ms(2)+off_ms(2)` = **8 字节**  
**触发:** LoRa 下行 CMD 0x05 发送恰好 7 字节 payload  
**修复:**
```c
if (len < 8) return -EINVAL;
```

### 3. proto_handler.c:157 — handle_buzzer_control 越界读

**位置:** `firmware/src/proto/proto_handler.c:157-159`

```c
if (len < 5) return -EINVAL;                     // ❌ 应该是 len < 6
return actuator_buzzer_override(data[0], data[1],
    (data[2]<<8)|data[3], (data[4]<<8)|data[5]);  // 读 data[5] 需要 6 字节
```

**格式:** `mode(1)+volume(1)+on_ms(2)+off_ms(2)` = **6 字节**  
**触发:** LoRa 下行 CMD 0x06 发送恰好 5 字节 payload  
**修复:**
```c
if (len < 6) return -EINVAL;
```

### 4. actuator_mgr.c:234,241,247 — 网络输入未校验枚举转换

**位置:** `firmware/src/app/actuator_mgr.c:229-248`

```c
// 三个 override 函数将网络 uint8_t mode 直接强转为 enum，无范围校验
led_strip_set_mode((enum led_mode)mode, ...);      // line 234
buzzer_pwm_set((enum buzzer_mode)mode, ...);        // line 241
vibration_set((enum vibration_mode)mode, ...);      // line 247
```

**影响:** `led_strip_set_mode` switch 未显式处理的 mode 值落入 default 分支（行为未定义）。buzzer/vibration 有 default 返回 -EINVAL 相对安全。  
**修复:** ✅ **已修复 (2026-07-10)** — 在 actuator_led/buzzer/vibration_override 函数入口增加 mode 范围校验：
```c
// actuator_led_override:    if (mode > LED_MODE_FAST) return -EINVAL;   // 0~4
// actuator_buzzer_override: if (mode > BUZZER_PATTERN) return -EINVAL;  // 0~2
// actuator_vibration_override: if (mode > VIB_PATTERN) return -EINVAL;  // 0~2
```

---

## 🔵 Medium (2)

### 5. proto_handler.c:177 — handle_lcd_content 无长度校验

```c
static int handle_lcd_content(const uint8_t *data, uint8_t len)
{
    LOG_INF("CMD 0x08 LCD: line=%d len=%d (stub)", data[0], len - 1);  // 未检查 len>=1
    return 0;
}
```

**修复:** ✅ **已修复 (协议对齐重写)** — 增加 `if (len < 1) return -EINVAL;`，含 Group id 字段和多播过滤

### 6. proto_handler.c:186 — handle_lcd_line2_onoff 无长度校验

```c
static int handle_lcd_line2_onoff(const uint8_t *data, uint8_t len)
{
    LOG_INF("CMD 0x09 LCD Line2: %s (stub)", data[0] ? "ON" : "OFF");  // 未检查 len>=1
    return 0;
}
```

**修复:** ✅ **已修复 (协议对齐重写)** — 增加 `if (len < 2) return -EINVAL;`，修正为 data[0]=Group, data[1]=enable

---

## ⚪ Low (3)

### 7. proto_handler.c:106 — alarm_type>=16 绕过校验覆写 Normal 配置

`alarm_type_to_priority()` 对 `alarm_type >= 16` 返回 `ALARM_PRIO_NORMAL(8)`，而 `prio >= 9` 的检查捕获不到，导致 CMD 0x04 可以用任意 alarm_type 值覆写 Normal 槽位配置。

**修复:** ✅ **已修复 (协议对齐重写)** — `proto_alarm_to_internal()` 对无效值返回 `0xFF`，调用方检查后返回 `-EINVAL`。同时 `alarm_type_to_priority()` 返回值仍有 `prio >= ALARM_PRIO_MAX` 检查。

### 8. drv/*.c tick 函数 — 64→16bit 时间截断

`buzzer_pwm.c:128`, `vibration_gpio.c:62`, `led_pwm.c:60`, `led_strip.c:154` 将 64bit `k_uptime_get()` 差值截断为 `uint16_t`（最大 65.5 秒）。auto_stop 在 30/60s 触发后会先关闭，实际影响有限。

**修复 (可选):** ✅ **已修复 (2026-07-10)** — 四个驱动文件 tick 函数全部改为 `uint32_t` 截断（最大 49 天）：
```c
uint32_t elap = (uint32_t)(now - last_toggle_ms);
```
影响文件: `buzzer_pwm.c`, `vibration_gpio.c`, `led_pwm.c`, `led_strip.c`

### 9. badge_ui.c:234 — button_id 无防御性边界检查

```c
uint8_t alarm_type = button_to_alarm[button_id];  // button_id 当前总是 0-3
```

当前调用者保证了范围，但作为公共回调缺少防御性检查。  
**修复:** ✅ **已修复 (2026-07-10)** — 在 `on_button_event()` 入口增加 `if (button_id >= BUTTON_COUNT) { LOG_WRN(...); return; }`

---

## 汇总表

| # | 严重级别 | 文件 | 行号 | 问题 | 状态 |
|---|---------|------|------|------|------|
| 1 | 🔴 Critical | proto/proto_builder.c | 68 | `data[16]` 写 17 字节栈溢出 | ✅ 已修复 |
| 2 | 🟡 High | proto/proto_handler.c | 147 | handle_led_control `len<7`→`len<8` | ✅ 已修复 (协议对齐重写) |
| 3 | 🟡 High | proto/proto_handler.c | 157 | handle_buzzer_control `len<5`→`len<6` | ✅ 已修复 (协议对齐重写) |
| 4 | 🟡 High | app/actuator_mgr.c | 234 | 网络 mode 值无校验强转枚举 | ✅ 已修复 (2026-07-10) |
| 5 | 🔵 Medium | proto/proto_handler.c | 177 | handle_lcd_content 缺 len 检查 | ✅ 已修复 (协议对齐重写) |
| 6 | 🔵 Medium | proto/proto_handler.c | 186 | handle_lcd_line2_onoff 缺 len 检查 | ✅ 已修复 (协议对齐重写) |
| 7 | ⚪ Low | proto/proto_handler.c | 106 | alarm_type>=16 绕过校验 | ✅ 已修复 (proto_alarm_to_internal 校验) |
| 8 | ⚪ Low | drv/buzzer_pwm.c 等 | 128 | 64→16bit 时间截断 | ✅ 已修复 (2026-07-10, uint16_t→uint32_t) |
| 9 | ⚪ Low | ui/badge_ui.c | 234 | button_id 缺防御性检查 | ✅ 已修复 (2026-07-10) |

---

## 修复优先级建议

~~1. **立即修复:** #2, #3, #5, #6（远程触发 OOB 读 + 缺长度检查，5 分钟改完）~~
~~2. **本次迭代:** #4, #7（枚举校验 + 类型绕过）~~
~~3. **后续优化:** #8, #9（防御性加固）~~

**✅ 全部 9 个问题已于 2026-07-10 修复完成。**

修复批次:
- **协议对齐重写** (2026-07-09): #1, #2, #3, #5, #6, #7
- **2026-07-10 补充修复**: #4, #8, #9
  - #4: `actuator_mgr.c` 三个 override 函数增加 mode 枚举范围校验
  - #8: `buzzer_pwm.c`, `vibration_gpio.c`, `led_pwm.c`, `led_strip.c` tick 函数 `uint16_t`→`uint32_t`
  - #9: `badge_ui.c` `on_button_event()` 入口增加 `button_id >= BUTTON_COUNT` 防御检查
