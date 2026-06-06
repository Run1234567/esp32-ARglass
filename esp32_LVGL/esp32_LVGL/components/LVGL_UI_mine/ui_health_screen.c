#include "ui_health_screen.h"
#include "ui_manager.h"
#include "max30102.h"
#include "esp_lvgl_port.h"

lv_obj_t * ui_health_screen;
static lv_obj_t * arc_bpm;
static lv_obj_t * label_bpm_val;
static lv_obj_t * label_spo2_val;
static lv_timer_t * update_timer;

static void update_health_data_cb(lv_timer_t * timer) {
    if (lv_scr_act() != ui_health_screen) return;

    if (lvgl_port_lock(0)) {
        float bpm = max30102_get_bpm();
        float spo2 = max30102_get_spo2();

        lv_label_set_text_fmt(label_bpm_val, "%d", (int)bpm);
        lv_arc_set_value(arc_bpm, (int)bpm);
        lv_label_set_text_fmt(label_spo2_val, "%d%%", (int)spo2);

        lvgl_port_unlock();
    }
}

void health_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}

void ui_health_screen_init(void) {
    ui_health_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_health_screen, lv_color_black(), 0);
    lv_obj_set_style_border_width(ui_health_screen, 0, 0);

    // 顶部标题
    lv_obj_t * title = lv_label_create(ui_health_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "生理监控");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 心率青色光环
    arc_bpm = lv_arc_create(ui_health_screen);
    lv_obj_set_size(arc_bpm, 130, 130);
    lv_arc_set_range(arc_bpm, 40, 160);
    lv_arc_set_rotation(arc_bpm, 135);
    lv_arc_set_bg_angles(arc_bpm, 0, 270);
    lv_obj_align(arc_bpm, LV_ALIGN_CENTER, -40, 5);
    lv_obj_remove_style(arc_bpm, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc_bpm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc_bpm, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_bpm, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);

    label_bpm_val = lv_label_create(arc_bpm);
    lv_obj_set_style_text_font(label_bpm_val, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_bpm_val, lv_color_white(), 0);
    lv_label_set_text(label_bpm_val, "--");
    lv_obj_center(label_bpm_val);

    // 血氧橙色显示区
    lv_obj_t * label_spo2_title = lv_label_create(ui_health_screen);
    lv_obj_set_style_text_font(label_spo2_title, &my_font_cn_16, 0);
    lv_label_set_text(label_spo2_title, "SpO2");
    lv_obj_set_style_text_color(label_spo2_title, lv_color_hex(0x888888), 0);
    lv_obj_align(label_spo2_title, LV_ALIGN_CENTER, 65, -15);

    label_spo2_val = lv_label_create(ui_health_screen);
    lv_obj_set_style_text_font(label_spo2_val, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_spo2_val, lv_color_hex(0xFF973B), 0);
    lv_label_set_text(label_spo2_val, "--%");
    lv_obj_align(label_spo2_val, LV_ALIGN_CENTER, 65, 15);

    // 底部提示
    lv_obj_t * tip = lv_label_create(ui_health_screen);
    lv_obj_set_style_text_font(tip, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(tip, lv_color_hex(0x888888), 0);
    lv_label_set_text(tip, "左挥返回");
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -15);

    // 定时器 0.5 秒刷新一次
    update_timer = lv_timer_create(update_health_data_cb, 500, NULL);
}
