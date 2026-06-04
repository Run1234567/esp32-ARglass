#include "ui_record_screen.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "my_uart.h" // ✨ 引入串口发送模块 
#include <stdio.h>

static const char *TAG = "UI_RECORD";

lv_obj_t * ui_record_screen;
static lv_obj_t * label_title;
static lv_obj_t * label_record_time;
static lv_obj_t * label_status;

static lv_timer_t * record_timer = NULL;
static uint8_t is_recording = 0;      // 状态标记
static uint32_t record_time_sec = 0;  // 录制时长

// ==========================================
// ?? 录音计时器回调 (1秒触发一次)
// ==========================================
static void record_timer_cb(lv_timer_t * timer) {
    if (is_recording) {
        record_time_sec++;
        
        // 更新大数字时间
        lv_label_set_text_fmt(label_record_time, "%02d:%02d", (int)(record_time_sec / 60), (int)(record_time_sec % 60));

        // ? 闪烁动画效果：通过切换颜色的深浅，做出“呼吸/录制”的动效
        if (record_time_sec % 2 == 0) {
            lv_label_set_text(label_status, "#FF0000 ? 录音中...#\n右滑停止");
        } else {
            lv_label_set_text(label_status, "#AA0000 ? 录音中...#\n右滑停止"); // 暗红色
        }
    }
}

// ==========================================
// ? 核心手势路由
// ==========================================
void record_screen_handle_cmd(ui_cmd_t cmd) {
    
    if (cmd == UI_CMD_RIGHT) {
        if (!is_recording) {
            // ✨ 触发：开始录音
            is_recording = 1;
            record_time_sec = 0;
            lv_obj_set_style_text_color(label_record_time, lv_color_hex(0xFF0000), 0); // 数字变红
            lv_label_set_text(label_record_time, "00:00");
            lv_label_set_text(label_status, "#FF0000 🔴 录音中...#\n右滑停止");
            lv_timer_resume(record_timer);
            
            ESP_LOGI(TAG, "🎙️ 界面触发：开始录音!");
            
            // 🚀 核心：通过串口通知外设板开始录音 
            my_uart_send("CMD:REC_START\r\n"); 
            
        } else {
            // ✨ 触发：停止录音
            is_recording = 0;
            lv_timer_pause(record_timer);
            lv_obj_set_style_text_color(label_record_time, lv_color_hex(0x00FF00), 0); // 数字变变绿
            lv_label_set_text(label_status, "#00FF00 ✅ 录音完成#\n右滑重录 | 左滑退出");
            
            ESP_LOGI(TAG, "⏹️ 界面触发：录音结束"); 
            
            // 🚀 核心：通过串口通知外设板停止录音 
            my_uart_send("CMD:REC_STOP\r\n"); 
        }
    }
    else if (cmd == UI_CMD_LEFT) {
        // ✨ 左滑退出：为了防止误触，录音期间不允许直接退出
        if (is_recording) {
            ESP_LOGW(TAG, "⚠️ 录音中，屏蔽左滑退出指令");
        } else {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU); // 退回主菜单
            
            // 退出时恢复界面的初始状态
            lv_obj_set_style_text_color(label_record_time, lv_color_white(), 0);
            lv_label_set_text(label_record_time, "00:00");
            lv_label_set_text(label_status, "准备就绪\n右滑开始录音");
        }
    }
}

// ==========================================
// ? 初始化界面
// ==========================================
void ui_record_screen_init(void) {
    ui_record_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_record_screen, lv_color_black(), 0);

    // 1. 顶部标题
    label_title = lv_label_create(ui_record_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_white(), 0);
    lv_label_set_text(label_title, "?? 语音备忘录");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    // 2. 居中大数字时间 (借用系统英文字体)
    label_record_time = lv_label_create(ui_record_screen);
    lv_obj_set_style_text_font(label_record_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_record_time, lv_color_white(), 0);
    lv_label_set_text(label_record_time, "00:00");
    lv_obj_align(label_record_time, LV_ALIGN_CENTER, 0, -10);

    // 3. 底部状态提示词 (必须开启颜色重绘)
    label_status = lv_label_create(ui_record_screen);
    lv_label_set_recolor(label_status, true);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_status, "准备就绪\n右滑开始录音");
    lv_obj_align(label_status, LV_ALIGN_CENTER, 0, 45);

    // 4. 创建定时器，默认挂起不跑
    record_timer = lv_timer_create(record_timer_cb, 1000, NULL);
    lv_timer_pause(record_timer);
}
