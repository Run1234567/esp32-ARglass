/**
 * @file ui_translate_lang_screen.c
 * @brief 翻译语言选择界面 —— 根据模式动态显示语言列表
 *
 * S2S 同传模式：14项互译列表
 * S2T 字幕模式：8项语言对列表（源语言 -> 目标语言）
 */

#include "ui_translate_lang_screen.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "ui_translate_screen.h"
#include "my_uart.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>

static const char *TAG = "TRANSLATE_LANG";

lv_obj_t * ui_translate_lang_screen;
static lv_obj_t * lang_roller;

static int current_translate_mode = 0; // 0 = S2S同传, 1 = S2T字幕

// --- 动态设置选项的 API ---
void ui_translate_lang_set_mode(int mode) {
    current_translate_mode = mode;
    if (lang_roller == NULL) return;

    if (mode == 0) {
        // S2S 同传模式：互译列表 (14项)
        lv_roller_set_options(lang_roller,
            "中文 -> 英文\n英文 -> 中文\n"
            "中文 -> 日文\n日文 -> 中文\n"
            "中文 -> 葡萄牙语\n葡萄牙语 -> 中文\n"
            "中文 -> 西班牙语\n西班牙语 -> 中文\n"
            "中文 -> 印尼语\n印尼语 -> 中文\n"
            "中文 -> 德语\n德语 -> 中文\n"
            "中文 -> 法语\n法语 -> 中文",
            LV_ROLLER_MODE_NORMAL);
    } else {
        // S2T 字幕模式：明确指定源语言 -> 目标语言 (8项)
        // 火山引擎要求合法的 langPair，不能自转自
        lv_roller_set_options(lang_roller,
            "中文 (转英)\n英文 (转中)\n"
            "日文 (转中)\n韩文 (转中)\n"
            "粤语 (转中)\n法语 (转中)\n"
            "德语 (转中)\n西语 (转中)",
            LV_ROLLER_MODE_NORMAL);
    }
    lv_roller_set_selected(lang_roller, 0, LV_ANIM_OFF);
}

void ui_translate_lang_screen_init(void) {
    ui_translate_lang_screen = lv_obj_create(NULL);
    if (ui_translate_lang_screen == NULL) {
        ESP_LOGE(TAG, "LVGL 内存不足！无法创建翻译语言界面！");
        return;
    }
    lv_obj_set_style_bg_color(ui_translate_lang_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_translate_lang_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_translate_lang_screen, 0, 0);

    // ---- 标题 ----
    lv_obj_t * title = lv_label_create(ui_translate_lang_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "选择翻译语言");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // ---- 语言选择滚轮 ----
    lang_roller = lv_roller_create(ui_translate_lang_screen);
    lv_obj_set_style_text_font(lang_roller, &my_font_cn_16, 0);
    lv_roller_set_options(lang_roller, "加载中...", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(lang_roller, 4);
    lv_obj_align(lang_roller, LV_ALIGN_CENTER, 0, 10);

    // 滚轮样式
    lv_obj_set_style_bg_opa(lang_roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lang_roller, 0, 0);
    lv_obj_set_style_text_color(lang_roller, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(lang_roller, lv_color_hex(0x00FFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(lang_roller, LV_OPA_TRANSP, LV_PART_SELECTED);

    // ---- 底部提示 ----
    lv_obj_t * hint = lv_label_create(ui_translate_lang_screen);
    lv_obj_set_style_text_font(hint, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_label_set_text(hint, "右挥确认 | 左挥返回");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void translate_lang_screen_handle_cmd(int cmd) {
    uint16_t max_options = (current_translate_mode == 0) ? 14 : 8;

    if (cmd == UI_CMD_UP) {
        uint16_t sel = lv_roller_get_selected(lang_roller);
        if (sel > 0) lv_roller_set_selected(lang_roller, sel - 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_DOWN) {
        uint16_t sel = lv_roller_get_selected(lang_roller);
        if (sel < max_options - 1) lv_roller_set_selected(lang_roller, sel + 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_RIGHT || cmd == UI_CMD_FORWARD) {
        uint16_t sel = lv_roller_get_selected(lang_roller);
        char cmd_buf[64];

        if (current_translate_mode == 0) {
            // ---- 同传模式 (S2S) ----
            const char *src = "zh", *tgt = "en";
            switch(sel) {
                case 0: src="zh"; tgt="en"; break;  case 1: src="en"; tgt="zh"; break;
                case 2: src="zh"; tgt="ja"; break;  case 3: src="ja"; tgt="zh"; break;
                case 4: src="zh"; tgt="pt"; break;  case 5: src="pt"; tgt="zh"; break;
                case 6: src="zh"; tgt="es"; break;  case 7: src="es"; tgt="zh"; break;
                case 8: src="zh"; tgt="id"; break;  case 9: src="id"; tgt="zh"; break;
                case 10: src="zh"; tgt="de"; break; case 11: src="de"; tgt="zh"; break;
                case 12: src="zh"; tgt="fr"; break; case 13: src="fr"; tgt="zh"; break;
            }
            snprintf(cmd_buf, sizeof(cmd_buf), "CMD:SET_LANG:%s:%s\r\n", src, tgt);
            my_uart_send(cmd_buf);
            my_uart_send("CMD:SET_MODE:s2s\r\n");
            ESP_LOGI(TAG, "S2S模式: %s -> %s", src, tgt);
        } else {
            // ---- 字幕模式 (S2T) ----
            // 火山引擎要求合法的 langPair，不能自转自
            const char *src = "zh", *tgt = "en";
            switch(sel) {
                case 0: src="zh"; tgt="en"; break; // 中文识别 -> 英文
                case 1: src="en"; tgt="zh"; break; // 英文识别 -> 中文
                case 2: src="ja"; tgt="zh"; break; // 日文识别 -> 中文
                case 3: src="ko"; tgt="zh"; break; // 韩文识别 -> 中文
                case 4: src="yue";tgt="zh"; break; // 粤语识别 -> 中文
                case 5: src="fr"; tgt="zh"; break; // 法文识别 -> 中文
                case 6: src="de"; tgt="zh"; break; // 德文识别 -> 中文
                case 7: src="es"; tgt="zh"; break; // 西语识别 -> 中文
            }
            snprintf(cmd_buf, sizeof(cmd_buf), "CMD:SET_LANG:%s:%s\r\n", src, tgt);
            my_uart_send(cmd_buf);
            my_uart_send("CMD:SET_MODE:s2t\r\n");
            ESP_LOGI(TAG, "S2T模式: %s -> %s", src, tgt);
        }

        // 进入翻译字幕界面并启动翻译
        switch_to_screen(SCREEN_TRANSLATE);
    }
    else if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        // 左滑退回到模式选择界面
        switch_to_screen(SCREEN_TRANSLATE_MODE);
    }
}
