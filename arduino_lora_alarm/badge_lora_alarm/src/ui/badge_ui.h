/*
 * Badge UI — 两段式确认 + 按键→告警映射 + 上行触发
 * 移植自 NCS: badge_ui.c (移除 OLED/BLE/GPS)
 *
 * 确认流程:
 *   短按: 显示电池, 不触发告警
 *   长按 (≥3s): "Hold 2s" 确认 → 继续 2s → 触发告警
 *   组合键 (5s): 立即执行, 无需确认
 */
#ifndef BADGE_UI_H
#define BADGE_UI_H

int  badge_ui_init(void);
void badge_ui_poll(void);
void badge_ui_show_join_status(int state);
void badge_ui_show_alarm(void);
void badge_ui_set_lcd_content(const char *line1, const char *line2);
void badge_ui_set_lcd_line2_visible(bool visible);
void badge_ui_clear_display(void);

#endif /* BADGE_UI_H */
