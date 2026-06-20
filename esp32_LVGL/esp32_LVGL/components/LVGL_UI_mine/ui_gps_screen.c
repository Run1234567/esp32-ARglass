/**
 * @file ui_gps_screen.c
 * @brief GPS 定位界面 —— 显示经纬度、海拔、速度、卫星数
 */

#include "ui_gps_screen.h"
#include "ui_manager.h"
#include "gps.h"
#include <math.h>
#include <stdio.h>

lv_obj_t * ui_gps_screen;
static lv_obj_t * label_status;
static lv_obj_t * label_lat_lon;
static lv_obj_t * label_alt_spd;
static lv_obj_t * label_gps_time;
static lv_timer_t * gps_timer = NULL;

// ============================================================
//   定时器回调（LVGL 定时器内部已持锁，无需再加锁）
// ============================================================
static void gps_update_cb(lv_timer_t * timer) {
    if (lv_scr_act() != ui_gps_screen) return;

    gps_data_t data = gps_get_data();
    char buf[128];

    if (data.valid) {
        lv_label_set_text_fmt(label_status, "已定位 (卫星:%d)", data.satellites);
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FF00), 0);

        snprintf(buf, sizeof(buf), "纬度: %.5f %c\n经度: %.5f %c",
                 fabs(data.latitude), data.latitude >= 0 ? 'N' : 'S',
                 fabs(data.longitude), data.longitude >= 0 ? 'E' : 'W');
        lv_label_set_text(label_lat_lon, buf);

        snprintf(buf, sizeof(buf), "海拔: %.1f m\n速度: %.1f km/h",
                 data.altitude, data.speed_kmh);
        lv_label_set_text(label_alt_spd, buf);

        lv_label_set_text_fmt(label_gps_time, "时间: %s", data.utc_time);
    } else {
        lv_label_set_text_fmt(label_status, "搜星中... (卫星:%d)", data.satellites);
        lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFA500), 0);

        lv_label_set_text(label_lat_lon, "纬度: --\n经度: --");
        lv_label_set_text(label_alt_spd, "海拔: -- m\n速度: -- km/h");
        lv_label_set_text(label_gps_time, "时间: --:--:--");
    }
}

// ============================================================
//   启停刷新
// ============================================================
void ui_gps_start_update(void) {
    if (gps_timer == NULL) {
        gps_timer = lv_timer_create(gps_update_cb, 1000, NULL);
    }
    gps_update_cb(NULL);
}

void ui_gps_stop_update(void) {
    if (gps_timer != NULL) {
        lv_timer_del(gps_timer);
        gps_timer = NULL;
    }
}

// ============================================================
//   界面初始化
// ============================================================
void ui_gps_screen_init(void) {
    ui_gps_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_gps_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_gps_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_gps_screen, 0, 0);

    label_status = lv_label_create(ui_gps_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
    lv_label_set_text(label_status, "准备获取 GPS...");
    lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 15);

    label_lat_lon = lv_label_create(ui_gps_screen);
    lv_obj_set_style_text_font(label_lat_lon, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_lat_lon, lv_color_white(), 0);
    lv_label_set_text(label_lat_lon, "纬度: --\n经度: --");
    lv_obj_align(label_lat_lon, LV_ALIGN_TOP_LEFT, 20, 60);

    label_alt_spd = lv_label_create(ui_gps_screen);
    lv_obj_set_style_text_font(label_alt_spd, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_alt_spd, lv_color_white(), 0);
    lv_label_set_text(label_alt_spd, "海拔: -- m\n速度: -- km/h");
    lv_obj_align(label_alt_spd, LV_ALIGN_TOP_LEFT, 20, 110);

    label_gps_time = lv_label_create(ui_gps_screen);
    lv_obj_set_style_text_font(label_gps_time, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_gps_time, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(label_gps_time, "时间: --:--:--");
    lv_obj_align(label_gps_time, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ============================================================
//   手势处理
// ============================================================
void gps_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}
