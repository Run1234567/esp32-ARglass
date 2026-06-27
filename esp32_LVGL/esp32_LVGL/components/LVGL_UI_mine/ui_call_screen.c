/**
 * @file ui_call_screen.c
 * @brief 网络通话界面 —— 拨号 + 被呼叫（响铃）界面
 */

#include "ui_call_screen.h"
#include "ui_manager.h"
#include "my_uart.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "CALL_UI";

// 拨号界面对象
lv_obj_t * ui_call_screen;
static lv_obj_t * label_contact1;
static lv_obj_t * label_contact2;
static lv_obj_t * label_status;

static int selected_contact = 0;
volatile bool is_calling_now = false;

// 被呼叫（响铃）界面对象
lv_obj_t * ui_ring_screen = NULL;
static lv_obj_t * label_ring_title;
static lv_obj_t * label_ring_tips;
volatile bool is_ringing_now = false;

// ============================================================
//   刷新拨号界面联系人显示
// ============================================================
static void update_contact_ui(void) {
    if (is_calling_now) {
        lv_obj_set_style_text_color(label_contact1, lv_color_hex(0x555555), 0);
        lv_obj_set_style_text_color(label_contact2, lv_color_hex(0x555555), 0);
        return;
    }
    if (selected_contact == 0) {
        lv_label_set_text(label_contact1, "> 手机客户端");
        lv_obj_set_style_text_color(label_contact1, lv_color_white(), 0);
        lv_label_set_text(label_contact2, "  演示端");
        lv_obj_set_style_text_color(label_contact2, lv_color_hex(0x888888), 0);
    } else {
        lv_label_set_text(label_contact1, "  手机客户端");
        lv_obj_set_style_text_color(label_contact1, lv_color_hex(0x888888), 0);
        lv_label_set_text(label_contact2, "> 演示端");
        lv_obj_set_style_text_color(label_contact2, lv_color_white(), 0);
    }
}

// ============================================================
//   初始化拨号界面
// ============================================================
void ui_call_screen_init(void) {
    ui_call_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_call_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_call_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_call_screen, 0, 0);

    lv_obj_t * title = lv_label_create(ui_call_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00A8FF), 0);
    lv_label_set_text(title, "网络通话");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    label_contact1 = lv_label_create(ui_call_screen);
    lv_obj_set_style_text_font(label_contact1, &my_font_cn_16, 0);
    lv_obj_align(label_contact1, LV_ALIGN_CENTER, 0, -15);

    label_contact2 = lv_label_create(ui_call_screen);
    lv_obj_set_style_text_font(label_contact2, &my_font_cn_16, 0);
    lv_obj_align(label_contact2, LV_ALIGN_CENTER, 0, 15);

    label_status = lv_label_create(ui_call_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_status, "右滑呼叫 | 左滑退出");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -10);

    update_contact_ui();
}

// ============================================================
//   初始化响铃（被呼叫）界面
// ============================================================
static void ui_ring_screen_init(void) {
    ui_ring_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_ring_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_ring_screen, LV_OPA_COVER, 0);

    label_ring_title = lv_label_create(ui_ring_screen);
    lv_obj_set_style_text_font(label_ring_title, &my_font_cn_16, 0);
    lv_label_set_text(label_ring_title, "有新来电...");
    lv_obj_set_style_text_color(label_ring_title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(label_ring_title, LV_ALIGN_CENTER, 0, -20);

    label_ring_tips = lv_label_create(ui_ring_screen);
    lv_obj_set_style_text_font(label_ring_tips, &my_font_cn_16, 0);
    lv_label_set_text(label_ring_tips, "右滑接听\n左滑挂断");
    lv_obj_set_style_text_align(label_ring_tips, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_ring_tips, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(label_ring_tips, LV_ALIGN_CENTER, 0, 20);
}

// ============================================================
//   外部调用：进入响铃状态
// ============================================================
void ui_enter_ringing_mode(void) {
    if (ui_ring_screen == NULL) {
        ui_ring_screen_init();
    }
    is_ringing_now = true;
    is_calling_now = false;
    lv_scr_load(ui_ring_screen);
    ESP_LOGI(TAG, "进入响铃界面");
}

// ============================================================
//   手势处理
// ============================================================
void call_screen_handle_cmd(ui_cmd_t cmd) {

    // 优先处理响铃状态
    if (is_ringing_now) {
        if (cmd == UI_CMD_RIGHT) {
            // 接听
            is_ringing_now = false;
            is_calling_now = true;
            my_uart_send("CMD:CALL_ACCEPT\r\n");
            lv_scr_load(ui_call_screen);
            lv_label_set_text(label_status, "正在通话中...\n左滑挂断");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FF00), 0);
            update_contact_ui();
            ESP_LOGI(TAG, "接听电话");
        }
        else if (cmd == UI_CMD_LEFT) {
            // 拒绝
            is_ringing_now = false;
            is_calling_now = false;
            my_uart_send("CMD:CALL_END\r\n");
            lv_scr_load(ui_call_screen);
            lv_label_set_text(label_status, "右滑呼叫 | 左滑退出");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFF00), 0);
            update_contact_ui();
            ESP_LOGI(TAG, "拒绝来电");
        }
        return;
    }

    // 主动拨号逻辑
    if (cmd == UI_CMD_UP && !is_calling_now) {
        selected_contact = 0;
        update_contact_ui();
    }
    else if (cmd == UI_CMD_DOWN && !is_calling_now) {
        selected_contact = 1;
        update_contact_ui();
    }
    else if (cmd == UI_CMD_RIGHT) {
        if (!is_calling_now) {
            is_calling_now = true;
            lv_label_set_text(label_status, "正在通话中...\n左滑挂断");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FF00), 0);
            update_contact_ui();
            ESP_LOGI(TAG, "发起呼叫");
            my_uart_send("CMD:CALL_START\r\n");
        }
    }
    else if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        if (is_calling_now) {
            is_calling_now = false;
            lv_label_set_text(label_status, "右滑呼叫 | 左滑退出");
            lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFF00), 0);
            update_contact_ui();
            my_uart_send("CMD:CALL_END\r\n");
            ESP_LOGI(TAG, "挂断通话");
        } else {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU);
        }
    }
}
