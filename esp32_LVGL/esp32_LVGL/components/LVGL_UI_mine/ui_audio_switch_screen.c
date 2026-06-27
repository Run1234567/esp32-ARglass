/**
 * @file ui_audio_switch_screen.c
 * @brief 音频输出切换界面 —— 控制 GPIO 5 (喇叭) 和 GPIO 6 (骨传导)
 */

#include "ui_audio_switch_screen.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "AUDIO_SW";

#define GPIO_SPEAKER_EN  5
#define GPIO_BONE_EN     6

lv_obj_t * ui_audio_switch_screen;
static lv_obj_t * sw_speaker;
static lv_obj_t * sw_bone;
static lv_obj_t * row_spk;
static lv_obj_t * row_bone;
static int current_row = 0;

static void update_row_focus(void) {
    if (current_row == 0) {
        lv_obj_set_style_border_width(row_spk, 1, 0);
        lv_obj_set_style_border_color(row_spk, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_border_width(row_bone, 0, 0);
    } else {
        lv_obj_set_style_border_width(row_spk, 0, 0);
        lv_obj_set_style_border_width(row_bone, 1, 0);
        lv_obj_set_style_border_color(row_bone, lv_color_hex(0x00FF00), 0);
    }
}

void ui_audio_switch_screen_init(void) {
    // 初始化 GPIO 5/6 为输出模式
    gpio_reset_pin(GPIO_SPEAKER_EN);
    gpio_reset_pin(GPIO_BONE_EN);
    gpio_set_direction(GPIO_SPEAKER_EN, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_BONE_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_SPEAKER_EN, 1); // 默认喇叭开
    gpio_set_level(GPIO_BONE_EN, 0);    // 默认骨传导关
    ESP_LOGI(TAG, "GPIO 5(喇叭)/6(骨传导) 初始化完成");

    ui_audio_switch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_audio_switch_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_audio_switch_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_audio_switch_screen, 0, 0);

    // 标题
    lv_obj_t * label_title = lv_label_create(ui_audio_switch_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(label_title, "音频输出切换");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 10);

    // 第一行：普通喇叭
    row_spk = lv_obj_create(ui_audio_switch_screen);
    lv_obj_set_size(row_spk, 220, 40);
    lv_obj_align(row_spk, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_bg_color(row_spk, lv_color_black(), 0);
    lv_obj_set_style_border_width(row_spk, 1, 0);
    lv_obj_set_style_border_color(row_spk, lv_color_hex(0x00FF00), 0);
    lv_obj_clear_flag(row_spk, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_spk = lv_label_create(row_spk);
    lv_obj_set_style_text_font(lbl_spk, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(lbl_spk, lv_color_white(), 0);
    lv_label_set_text(lbl_spk, "普通喇叭");
    lv_obj_align(lbl_spk, LV_ALIGN_LEFT_MID, 10, 0);

    sw_speaker = lv_switch_create(row_spk);
    lv_obj_align(sw_speaker, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_state(sw_speaker, LV_STATE_CHECKED); // 默认开

    // 第二行：骨传导振子
    row_bone = lv_obj_create(ui_audio_switch_screen);
    lv_obj_set_size(row_bone, 220, 40);
    lv_obj_align(row_bone, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_set_style_bg_color(row_bone, lv_color_black(), 0);
    lv_obj_set_style_border_width(row_bone, 0, 0);
    lv_obj_set_style_border_color(row_bone, lv_color_hex(0x00FF00), 0);
    lv_obj_clear_flag(row_bone, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl_bone = lv_label_create(row_bone);
    lv_obj_set_style_text_font(lbl_bone, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(lbl_bone, lv_color_white(), 0);
    lv_label_set_text(lbl_bone, "骨传导振子");
    lv_obj_align(lbl_bone, LV_ALIGN_LEFT_MID, 10, 0);

    sw_bone = lv_switch_create(row_bone);
    lv_obj_align(sw_bone, LV_ALIGN_RIGHT_MID, -10, 0);

    // 提示
    lv_obj_t * lbl_tip = lv_label_create(ui_audio_switch_screen);
    lv_obj_set_style_text_font(lbl_tip, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(lbl_tip, lv_color_hex(0x888888), 0);
    lv_label_set_text(lbl_tip, "上下切换 | 右滑开关 | 左滑返回");
    lv_obj_align(lbl_tip, LV_ALIGN_BOTTOM_MID, 0, -5);
}

void audio_switch_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_UP || cmd == UI_CMD_DOWN) {
        current_row = (current_row == 0) ? 1 : 0;
        update_row_focus();
    }
    else if (cmd == UI_CMD_RIGHT) {
        if (current_row == 0) {
            // 切换喇叭
            bool is_on = lv_obj_has_state(sw_speaker, LV_STATE_CHECKED);
            if (is_on) {
                lv_obj_clear_state(sw_speaker, LV_STATE_CHECKED);
                gpio_set_level(GPIO_SPEAKER_EN, 0);
                ESP_LOGI(TAG, "喇叭关闭 (GPIO5 LOW)");
            } else {
                lv_obj_add_state(sw_speaker, LV_STATE_CHECKED);
                gpio_set_level(GPIO_SPEAKER_EN, 1);
                ESP_LOGI(TAG, "喇叭开启 (GPIO5 HIGH)");
            }
        } else {
            // 切换骨传导
            bool is_on = lv_obj_has_state(sw_bone, LV_STATE_CHECKED);
            if (is_on) {
                lv_obj_clear_state(sw_bone, LV_STATE_CHECKED);
                gpio_set_level(GPIO_BONE_EN, 0);
                ESP_LOGI(TAG, "骨传导关闭 (GPIO6 LOW)");
            } else {
                lv_obj_add_state(sw_bone, LV_STATE_CHECKED);
                gpio_set_level(GPIO_BONE_EN, 1);
                ESP_LOGI(TAG, "骨传导开启 (GPIO6 HIGH)");
            }
        }
    }
    else if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}
