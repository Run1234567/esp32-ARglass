#include "ui_light_screen.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "light_sensor.h"
#include <stdio.h>

lv_obj_t * ui_light_screen;
static lv_obj_t * lux_value_label;
static lv_obj_t * lux_arc;
static lv_timer_t * light_timer = NULL;

// 每秒刷新一次光照数据
static void light_update_cb(lv_timer_t * timer) {
    if (lv_scr_act() != ui_light_screen) return;

    if (lvgl_port_lock(0)) {
        float lux = light_sensor_get_lux();

        // 更新中心数字
        lv_label_set_text_fmt(lux_value_label, "%.1f", lux);

        // 更新环形仪表盘进度 (0~3000 Lux)
        int arc_val = (int)lux;
        if (arc_val > 3000) arc_val = 3000;
        lv_arc_set_value(lux_arc, arc_val);

        lvgl_port_unlock();
    }
}

void ui_light_screen_init(void) {
    ui_light_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_light_screen, lv_color_black(), 0);

    // 1. 顶部标题
    lv_obj_t * title = lv_label_create(ui_light_screen);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_label_set_text(title, "环境光照度 (LUX)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 2. 科技感环形仪表盘
    lux_arc = lv_arc_create(ui_light_screen);
    lv_obj_set_size(lux_arc, 160, 160);
    lv_arc_set_rotation(lux_arc, 135);
    lv_arc_set_bg_angles(lux_arc, 0, 270);
    lv_arc_set_range(lux_arc, 0, 3000);
    lv_obj_align(lux_arc, LV_ALIGN_CENTER, 0, 10);

    // 隐藏旋钮，变为纯显示组件
    lv_obj_remove_style(lux_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(lux_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(lux_arc, lv_color_hex(0x00FF00), LV_PART_INDICATOR);

    // 3. 中心数值文本
    lux_value_label = lv_label_create(ui_light_screen);
    lv_obj_set_style_text_color(lux_value_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(lux_value_label, &my_font_cn_16, 0);
    lv_label_set_text(lux_value_label, "0.0");
    lv_obj_align(lux_value_label, LV_ALIGN_CENTER, 0, 10);

    // 4. 底部退出提示
    lv_obj_t * tip = lv_label_create(ui_light_screen);
    lv_obj_set_style_text_color(tip, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(tip, &my_font_cn_16, 0);
    lv_label_set_text(tip, "左挥返回");
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -15);

    // 5. 定时器：每 1000ms 刷新一次
    light_timer = lv_timer_create(light_update_cb, 1000, NULL);
}

void light_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}
