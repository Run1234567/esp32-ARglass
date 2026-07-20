/**
 * @file ui_step_screen.c
 * @brief 计步器界面 —— 实时显示步数
 */

#include "ui_step_screen.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>

static const char *TAG = "STEP_SCREEN";

lv_obj_t * ui_step_screen;
static lv_obj_t * label_step_value;
static lv_obj_t * label_step_icon;
static lv_obj_t * label_hint;
static lv_timer_t * step_timer;

// 定时器回调：定期读取 step_count 并刷新 UI
static void step_update_timer_cb(lv_timer_t * timer) {
    extern uint32_t step_count;
    char buf[32];
    sprintf(buf, "%lu", step_count);
    lv_label_set_text(label_step_value, buf);
}

void ui_step_screen_init(void) {
    ui_step_screen = lv_obj_create(NULL);
    if (ui_step_screen == NULL) {
        ESP_LOGE(TAG, "LVGL 内存不足！无法创建计步器界面！");
        return;
    }
    lv_obj_set_style_bg_color(ui_step_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_step_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_step_screen, 0, 0);

    // ---- 标题 ----
    lv_obj_t * label_title = lv_label_create(ui_step_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(label_title, "今日步数");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    // ---- 步数图标 ----
    label_step_icon = lv_label_create(ui_step_screen);
    lv_obj_set_style_text_font(label_step_icon, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_step_icon, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(label_step_icon, LV_SYMBOL_DUMMY " 走路");
    lv_obj_align(label_step_icon, LV_ALIGN_CENTER, 0, -40);

    // ---- 步数显示数值 ----
    label_step_value = lv_label_create(ui_step_screen);
    lv_obj_set_style_text_font(label_step_value, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_step_value, lv_color_white(), 0);
    lv_label_set_text(label_step_value, "0");
    lv_obj_align(label_step_value, LV_ALIGN_CENTER, 0, 0);

    // ---- "步" 单位标签 ----
    lv_obj_t * label_unit = lv_label_create(ui_step_screen);
    lv_obj_set_style_text_font(label_unit, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_unit, lv_color_hex(0x888888), 0);
    lv_label_set_text(label_unit, "步");
    lv_obj_align(label_unit, LV_ALIGN_CENTER, 0, 40);

    // ---- 底部提示 ----
    label_hint = lv_label_create(ui_step_screen);
    lv_obj_set_style_text_font(label_hint, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x666666), 0);
    lv_label_set_text(label_hint, "左挥返回 | 画圈回主页");
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 创建 LVGL 定时器，每 500ms 刷新一次界面步数
    step_timer = lv_timer_create(step_update_timer_cb, 500, NULL);
}
