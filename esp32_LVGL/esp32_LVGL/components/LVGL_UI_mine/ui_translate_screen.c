/**
 * @file ui_translate_screen.c
 * @brief 翻译模式界面 —— 实时显示原文和译文
 *
 * 串口指令：
 *   CMD:TRANSLATE_START  → 进入翻译模式，开始接收翻译数据
 *   CMD:TRANSLATE_STOP   → 退出翻译模式，返回主页
 *
 * 数据格式（UART 接收）：
 *   TRS:原文文本\r\n     → 原文
 *   TRT:译文文本\r\n     → 译文
 */

#include "ui_translate_screen.h"
#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "translate_control.h"
#include <string.h>

static const char *TAG = "TRANSLATE";

// ---- 全局屏幕对象 ----
lv_obj_t * ui_translate_screen;

// ---- 内部 UI 对象 ----
static lv_obj_t * label_status;      // 顶部状态栏
static lv_obj_t * label_src_title;   // "原文" 标题
static lv_obj_t * label_src_text;    // 原文内容
static lv_obj_t * label_dst_title;   // "译文" 标题
static lv_obj_t * label_dst_text;    // 译文内容
static lv_obj_t * label_hint;        // 底部操作提示

// ---- 翻译状态 ----
static bool translate_active = false;

// =========================================================
//   翻译界面初始化
// =========================================================
void ui_translate_screen_init(void) {
    ui_translate_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_translate_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_translate_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_translate_screen, 0, 0);

    // ---- 顶部状态栏 ----
    label_status = lv_label_create(ui_translate_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
    lv_label_set_text(label_status, LV_SYMBOL_CLOSE " 翻译模式");
    lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 8);

    // ---- 原文区域 ----
    label_src_title = lv_label_create(ui_translate_screen);
    lv_obj_set_style_text_font(label_src_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_src_title, lv_color_hex(0x00BFFF), 0);  // 深蓝色
    lv_label_set_text(label_src_title, "原文");
    lv_obj_align(label_src_title, LV_ALIGN_TOP_LEFT, 10, 32);

    label_src_text = lv_label_create(ui_translate_screen);
    lv_obj_set_width(label_src_text, 220);
    lv_label_set_long_mode(label_src_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label_src_text, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_src_text, lv_color_white(), 0);
    lv_label_set_text(label_src_text, "等待语音输入...");
    lv_obj_align(label_src_text, LV_ALIGN_TOP_LEFT, 10, 52);

    // ---- 分隔线 ----
    lv_obj_t * separator = lv_obj_create(ui_translate_screen);
    lv_obj_set_size(separator, 220, 1);
    lv_obj_set_style_bg_color(separator, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(separator, 0, 0);
    lv_obj_set_style_pad_all(separator, 0, 0);
    lv_obj_align(separator, LV_ALIGN_TOP_LEFT, 10, 120);

    // ---- 译文区域 ----
    label_dst_title = lv_label_create(ui_translate_screen);
    lv_obj_set_style_text_font(label_dst_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_dst_title, lv_color_hex(0x00FF00), 0);  // 绿色
    lv_label_set_text(label_dst_title, "译文");
    lv_obj_align(label_dst_title, LV_ALIGN_TOP_LEFT, 10, 128);

    label_dst_text = lv_label_create(ui_translate_screen);
    lv_obj_set_width(label_dst_text, 220);
    lv_label_set_long_mode(label_dst_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label_dst_text, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_dst_text, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(label_dst_text, "...");
    lv_obj_align(label_dst_text, LV_ALIGN_TOP_LEFT, 10, 148);

    // ---- 底部提示 ----
    label_hint = lv_label_create(ui_translate_screen);
    lv_obj_set_style_text_font(label_hint, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x666666), 0);
    lv_label_set_text(label_hint, "左挥返回 | 画圈返回");
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// =========================================================
//   更新原文
// =========================================================
void ui_update_translate_src(const char *text) {
    if (label_src_text != NULL && lvgl_port_lock(0)) {
        lv_label_set_text(label_src_text, text);
        lvgl_port_unlock();
    }
}

// =========================================================
//   更新译文
// =========================================================
void ui_update_translate_dst(const char *text) {
    if (label_dst_text != NULL && lvgl_port_lock(0)) {
        lv_label_set_text(label_dst_text, text);
        lvgl_port_unlock();
    }
}

// =========================================================
//   更新翻译文本（统一接口，由串口调用）
// =========================================================
void ui_update_translate_text(const char *text) {
    if (text == NULL) return;

    // 受限语种过滤：日语、韩语等显示提示
    if (strncmp(text, "JAP:", 4) == 0 || strncmp(text, "KOR:", 4) == 0) {
        ui_update_translate_dst("该语种受限");
        return;
    }

    // 根据前缀区分原文/译文
    if (strncmp(text, "SRC:", 4) == 0) {
        ui_update_translate_src(text + 4);
    } else if (strncmp(text, "DST:", 4) == 0) {
        ui_update_translate_dst(text + 4);
    } else {
        // 默认当译文处理
        ui_update_translate_dst(text);
    }
}

// =========================================================
//   进入翻译模式
// =========================================================
void ui_enter_translate_mode(void) {
    translate_active = true;
    if (label_status != NULL && lvgl_port_lock(-1)) {
        lv_label_set_text(label_status, LV_SYMBOL_OK " 翻译中...");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FF00), 0);
        lv_label_set_text(label_src_text, "等待语音输入...");
        lv_label_set_text(label_dst_text, "...");
        lvgl_port_unlock();
    }
    // 通知翻译板启动翻译
    translate_start(NULL, 0);
    ESP_LOGI(TAG, "翻译模式已启动");
}

// =========================================================
//   退出翻译模式
// =========================================================
void ui_exit_translate_mode(void) {
    translate_active = false;
    if (label_status != NULL && lvgl_port_lock(-1)) {
        lv_label_set_text(label_status, LV_SYMBOL_CLOSE " 翻译已停止");
        lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
        lvgl_port_unlock();
    }
    // 通知翻译板停止翻译
    translate_stop();
    ESP_LOGI(TAG, "翻译模式已退出");
}

// =========================================================
//   手势/指令处理
// =========================================================
void translate_screen_handle_cmd(int cmd) {
    if (cmd == UI_CMD_LEFT || cmd == UI_CMD_CIRCLE || cmd == UI_CMD_BACKWARD) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MAIN_AR);
    }
}
