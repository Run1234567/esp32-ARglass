/**
 * @file ui_video_screen.c
 * @brief AR 录像机界面
 */

#include "ui_video_screen.h"
#include "ui_manager.h"
#include "my_uart.h"
#include "esp_log.h"

static const char *TAG = "UI_VIDEO";

lv_obj_t * ui_video_screen;
static lv_obj_t * label_status;
static lv_obj_t * label_tips;
static bool is_recording_video = false;

static void video_force_stop(void) {
    if (is_recording_video) {
        is_recording_video = false;
        my_uart_send("CMD:VIDEO_STOP\r\n");
        lv_label_set_text(label_status, "已停止保存");
        lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
        lv_label_set_text(label_tips, "右滑: 开始录像\n左滑: 退出");
        ESP_LOGI(TAG, "停止录制视频");
    }
}

void ui_video_screen_init(void) {
    ui_video_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_video_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_video_screen, LV_OPA_COVER, 0);

    lv_obj_t * title = lv_label_create(ui_video_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00A8FF), 0);
    lv_label_set_text(title, "AR 录像机");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    label_status = lv_label_create(ui_video_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_label_set_text(label_status, "待机中...");
    lv_obj_set_style_text_color(label_status, lv_color_white(), 0);
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, -10);

    label_tips = lv_label_create(ui_video_screen);
    lv_obj_set_style_text_font(label_tips, &my_font_cn_16, 0);
    lv_label_set_text(label_tips, "右滑: 开始录像\n左滑: 退出");
    lv_obj_set_style_text_align(label_tips, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_tips, lv_color_hex(0x888888), 0);
    lv_obj_align(label_tips, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void video_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_RIGHT) {
        if (!is_recording_video) {
            is_recording_video = true;
            my_uart_send("CMD:VIDEO_START\r\n");
            lv_label_set_text(label_status, "正在录像...");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF0000), 0);
            lv_label_set_text(label_tips, "右/左滑: 停止录像");
            ESP_LOGI(TAG, "开始录制视频");
        } else {
            video_force_stop();
        }
    }
    else if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        if (is_recording_video) {
            video_force_stop();
        } else {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU);
        }
    }
}
