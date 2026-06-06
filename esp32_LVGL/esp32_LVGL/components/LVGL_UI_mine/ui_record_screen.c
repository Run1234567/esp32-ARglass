#include "ui_record_screen.h"
#include "ui_manager.h"
#include "esp_log.h"
#include "my_uart.h" 

static const char *TAG = "UI_RECORD";

lv_obj_t * ui_record_screen;
static lv_obj_t * label_title;
static lv_obj_t * label_record_time;
static lv_obj_t * label_status;
static lv_obj_t * record_arc;         // ✨ 新增：动态科技光环图案

static lv_timer_t * record_timer = NULL;
static uint8_t is_recording = 0;      // 状态标记
static uint32_t record_time_sec = 0;  // 录制时长

// ==========================================
// ⏱️ 录音计时器回调 (1秒触发一次)
// ==========================================
static void record_timer_cb(lv_timer_t * timer) {
    if (is_recording) {
        record_time_sec++;
        
        // 更新大数字时间
        lv_label_set_text_fmt(label_record_time, "%02d:%02d", (int)(record_time_sec / 60), (int)(record_time_sec % 60));

        // ✨ 动态光环效果：让红色的圆弧不断“生长”，模拟录音进度或声波拾取
        int arc_value = (record_time_sec * 15) % 100; // 每秒增加15%，满100归零循环
        if (arc_value == 0) arc_value = 100; 
        lv_arc_set_value(record_arc, arc_value);

        // 🔴 闪烁动画效果：通过切换颜色的深浅，做出“呼吸”的动效
        if (record_time_sec % 2 == 0) {
            lv_label_set_text(label_status, "#FF0000 🔴 录音中...#\n右滑停止");
            lv_obj_set_style_arc_color(record_arc, lv_color_hex(0xFF0000), LV_PART_INDICATOR); // 亮红
        } else {
            lv_label_set_text(label_status, "#AA0000 🔴 录音中...#\n右滑停止"); 
            lv_obj_set_style_arc_color(record_arc, lv_color_hex(0x880000), LV_PART_INDICATOR); // 暗红
        }
    }
}

// ==========================================
// 🕹️ 核心手势路由
// ==========================================
void record_screen_handle_cmd(ui_cmd_t cmd) {
    
    if (cmd == UI_CMD_RIGHT) {
        if (!is_recording) {
            // ✨ 触发：开始录音
            is_recording = 1;
            record_time_sec = 0;
            
            // UI 变红，光环激活动态红色
            lv_obj_set_style_text_color(label_record_time, lv_color_hex(0xFF0000), 0); 
            lv_label_set_text(label_record_time, "00:00");
            lv_label_set_text(label_status, "#FF0000 🔴 录音中...#\n右滑停止");
            
            lv_obj_set_style_arc_color(record_arc, lv_color_hex(0xFF0000), LV_PART_INDICATOR); // 激活部分变红
            lv_arc_set_value(record_arc, 5); // 给一个初始的一小段圆弧

            lv_timer_resume(record_timer);
            ESP_LOGI(TAG, "🎙️ 界面触发：开始录音!");
            my_uart_send("CMD:REC_START\r\n"); 
            
        } else {
            // ✨ 触发：停止录音
            is_recording = 0;
            lv_timer_pause(record_timer);
            
            // UI 变绿，光环变满绿
            lv_obj_set_style_text_color(label_record_time, lv_color_hex(0x00FF00), 0); 
            lv_label_set_text(label_status, "#00FF00 ✅ 录音完成#\n右滑重录 | 左滑退出");
            
            lv_obj_set_style_arc_color(record_arc, lv_color_hex(0x00FF00), LV_PART_INDICATOR); // 光环变绿
            lv_arc_set_value(record_arc, 100); // 光环满圈
            
            ESP_LOGI(TAG, "⏹️ 界面触发：录音结束"); 
            my_uart_send("CMD:REC_STOP\r\n"); 
        }
    }
    else if (cmd == UI_CMD_LEFT) {
        if (is_recording) {
            ESP_LOGW(TAG, "⚠️ 录音中，屏蔽左滑退出指令");
        } else {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU); // 退回主菜单
            
            // 退出时恢复界面的初始状态 (科技蓝/白配色)
            lv_obj_set_style_text_color(label_record_time, lv_color_white(), 0);
            lv_label_set_text(label_record_time, "00:00");
            lv_label_set_text(label_status, "准备就绪\n右滑开始录音");
            
            lv_obj_set_style_arc_color(record_arc, lv_color_hex(0x00FFFF), LV_PART_INDICATOR); // 恢复为青色
            lv_arc_set_value(record_arc, 0); // 进度归零
        }
    }
}

// ==========================================
// 🎨 初始化界面
// ==========================================
void ui_record_screen_init(void) {
    ui_record_screen = lv_obj_create(NULL);
    // 纯黑背景
    lv_obj_set_style_bg_color(ui_record_screen, lv_color_black(), 0);
    lv_obj_set_style_border_width(ui_record_screen, 0, 0); // 移除边框

    // 1. 顶部标题
    label_title = lv_label_create(ui_record_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_white(), 0);
    lv_label_set_text(label_title, "🎙️ 语音备忘录");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 15);

    // ✨ 2. 新增图案：中心动态科技光环 (HUD风格)
    record_arc = lv_arc_create(ui_record_screen);
    lv_obj_set_size(record_arc, 140, 140); // 设定光环大小
    lv_obj_align(record_arc, LV_ALIGN_CENTER, 0, -5);
    lv_arc_set_bg_angles(record_arc, 0, 360); // 允许画整圆
    lv_arc_set_angles(record_arc, 0, 0);      // 初始激活角度为0
    lv_obj_remove_style(record_arc, NULL, LV_PART_KNOB); // 隐藏圆弧末端的控制小圆点
    lv_obj_clear_flag(record_arc, LV_OBJ_FLAG_CLICKABLE); // 禁止触摸拖动

    // 光环轨道颜色（暗灰色）
    lv_obj_set_style_arc_color(record_arc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(record_arc, 5, LV_PART_MAIN);
    // 光环激活部分颜色（默认青色，录音时变红）
    lv_obj_set_style_arc_color(record_arc, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(record_arc, 5, LV_PART_INDICATOR);

    // 3. 居中大数字时间 (置于光环正中心)
    label_record_time = lv_label_create(ui_record_screen);
    lv_obj_set_style_text_font(label_record_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_record_time, lv_color_white(), 0);
    lv_label_set_text(label_record_time, "00:00");
    lv_obj_align_to(label_record_time, record_arc, LV_ALIGN_CENTER, 0, 0);

    // 4. 底部状态提示词
    label_status = lv_label_create(ui_record_screen);
    lv_label_set_recolor(label_status, true);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_status, "准备就绪\n右滑开始录音");
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -15);

    // 5. 创建定时器，默认挂起
    record_timer = lv_timer_create(record_timer_cb, 1000, NULL);
    lv_timer_pause(record_timer);
}