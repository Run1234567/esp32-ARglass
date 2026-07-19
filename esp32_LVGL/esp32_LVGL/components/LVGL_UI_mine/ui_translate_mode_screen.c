/**
 * @file ui_translate_mode_screen.c
 * @brief 翻译模式选择界面 —— 选择同声传译或语音转文本
 */

#include "ui_translate_mode_screen.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "ui_translate_lang_screen.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "TRANSLATE_MODE";

lv_obj_t * ui_translate_mode_screen;
static lv_obj_t * mode_roller;

void ui_translate_mode_screen_init(void) {
    ui_translate_mode_screen = lv_obj_create(NULL);
    if (ui_translate_mode_screen == NULL) {
        ESP_LOGE(TAG, "LVGL 内存不足！无法创建翻译模式界面！");
        return;
    }
    lv_obj_set_style_bg_color(ui_translate_mode_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_translate_mode_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_translate_mode_screen, 0, 0);

    // ---- 标题 ----
    lv_obj_t * title = lv_label_create(ui_translate_mode_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "选择翻译模式");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---- 模式选择滚轮 ----
    mode_roller = lv_roller_create(ui_translate_mode_screen);
    lv_obj_set_style_text_font(mode_roller, &my_font_cn_16, 0);
    lv_roller_set_options(mode_roller,
        "同声传译 (双向语音)\n"
        "语音转文本 (纯字幕)",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(mode_roller, 3);
    lv_obj_align(mode_roller, LV_ALIGN_CENTER, 0, 10);

    // 滚轮样式
    lv_obj_set_style_bg_opa(mode_roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_roller, 0, 0);
    lv_obj_set_style_text_color(mode_roller, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(mode_roller, lv_color_hex(0x00FFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(mode_roller, LV_OPA_TRANSP, LV_PART_SELECTED);

    // ---- 底部提示 ----
    lv_obj_t * hint = lv_label_create(ui_translate_mode_screen);
    lv_obj_set_style_text_font(hint, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_label_set_text(hint, "右挥确认 | 左挥返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void translate_mode_screen_handle_cmd(int cmd) {
    if (cmd == UI_CMD_UP) {
        uint16_t sel = lv_roller_get_selected(mode_roller);
        if (sel > 0) lv_roller_set_selected(mode_roller, sel - 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_DOWN) {
        uint16_t sel = lv_roller_get_selected(mode_roller);
        if (sel < 1) lv_roller_set_selected(mode_roller, sel + 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_RIGHT || cmd == UI_CMD_FORWARD) {
        uint16_t sel = lv_roller_get_selected(mode_roller);
        // 通知语种界面：0是同传(S2S)，1是字幕(S2T)
        extern void ui_translate_lang_set_mode(int mode);
        ui_translate_lang_set_mode(sel);

        // 跳转到语种选择界面
        switch_to_screen(SCREEN_TRANSLATE_LANG);
    }
    else if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        // 左滑返回主页
        switch_to_screen(SCREEN_MAIN_AR);
    }
}
