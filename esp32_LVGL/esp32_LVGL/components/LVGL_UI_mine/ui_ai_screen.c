/**
 * @file ui_ai_screen.c
 * @brief AI 字幕界面 —— 收到 SUB: 指令时霸屏显示 AI 回复
 */

#include "ui_ai_screen.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"

lv_obj_t * ui_ai_screen;
static lv_obj_t * ui_ai_label;

void ui_ai_screen_init(void) {
    ui_ai_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_ai_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_ai_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_ai_screen, 0, 0);

    ui_ai_label = lv_label_create(ui_ai_screen);
    lv_obj_set_width(ui_ai_label, 220);
    lv_label_set_long_mode(ui_ai_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ui_ai_label, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(ui_ai_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(ui_ai_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ui_ai_label, "Jarvis ready...");
}

void ui_update_ai_text(const char *text) {
    if (ui_ai_label != NULL && lvgl_port_lock(0)) {
        lv_label_set_text(ui_ai_label, text);
        lvgl_port_unlock();
    }
}

void ai_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MAIN_AR);
    }
}
