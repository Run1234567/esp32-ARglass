#include "ui_playlist_screen.h"
#include "my_uart.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>
#include "ui_manager.h"

lv_obj_t * ui_playlist_screen;
static lv_obj_t * roller_playlist;
static lv_obj_t * label_status;
static lv_obj_t * bar_progress;
static lv_obj_t * playlist_time_label;
static lv_obj_t * deco_arc; // ✨ 新增：外围动态科技光环

static char playlist_options[1024] = "";

static uint8_t play_state = 0; // 0:停止, 1:播放, 2:暂停
static int current_sec = 0;
static int total_sec = 0;
static lv_timer_t * progress_timer = NULL;
static int arc_rotation = 0; // ✨ 光环旋转角度

static void update_time_label() {
    lv_label_set_text_fmt(playlist_time_label, "%02d:%02d / %02d:%02d",
        current_sec / 60, current_sec % 60, total_sec / 60, total_sec % 60);
    lv_bar_set_value(bar_progress, current_sec, LV_ANIM_ON);
}

// ==========================================
// ⏱️ 播放进度与动画计时器
// ==========================================
static void progress_timer_cb(lv_timer_t * timer) {
    if (play_state == 1) {
        if (current_sec < total_sec) {
            current_sec++;
            update_time_label();
        }
        // 光环旋转（如果 arc 存在）
        if (deco_arc != NULL) {
            arc_rotation = (arc_rotation + 15) % 360;
            lv_arc_set_rotation(deco_arc, arc_rotation);
        }
    }
}

void playlist_set_total_time(int t_sec) {
    if (lvgl_port_lock(0)) {
        total_sec = t_sec;
        current_sec = 0;
        play_state = 1;

        lv_bar_set_range(bar_progress, 0, total_sec);
        lv_obj_clear_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(playlist_time_label, LV_OBJ_FLAG_HIDDEN);

        // UI 状态切为：播放中 (青色主题)
        if (deco_arc != NULL) lv_obj_set_style_arc_color(deco_arc, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
        
        update_time_label();
        lv_timer_resume(progress_timer);
        lv_label_set_text(label_status, "#00FFFF ▶ 正在播放 | 右滑暂停#");
        lvgl_port_unlock();
    }
}

void playlist_update_progress(int cur_sec) {
    if (lvgl_port_lock(0)) {
        current_sec = cur_sec;
        if (current_sec > total_sec) current_sec = total_sec;
        update_time_label();
        lvgl_port_unlock();
    }
}

void playlist_clear(void) { playlist_options[0] = '\0'; }

void playlist_add_file(const char* filename) {
    char clean_name[64];
    strncpy(clean_name, filename, sizeof(clean_name) - 1);
    clean_name[sizeof(clean_name) - 1] = '\0';
    for (int i = 0; i < strlen(clean_name); i++) {
        if (clean_name[i] == '\r' || clean_name[i] == '\n') { clean_name[i] = '\0'; break; }
    }
    if (strlen(playlist_options) + strlen(clean_name) + 2 < sizeof(playlist_options)) {
        strcat(playlist_options, clean_name); strcat(playlist_options, "\n");
    }
}

void playlist_update_ui(void) {
    if (lvgl_port_lock(0)) {
        if (strlen(playlist_options) > 0) {
            playlist_options[strlen(playlist_options) - 1] = '\0';
            lv_roller_set_options(roller_playlist, playlist_options, LV_ROLLER_MODE_NORMAL);
            lv_label_set_text(label_status, "#00FF00 右滑播放 | 左滑退出#");
        } else {
            lv_roller_set_options(roller_playlist, "暂无录音", LV_ROLLER_MODE_NORMAL);
            lv_label_set_text(label_status, "#FF0000 列表为空 | 左滑退出#");
        }
        lvgl_port_unlock();
    }
}

// ==========================================
// 🕹️ 核心手势路由
// ==========================================
void playlist_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT) {
        if (play_state != 0) {
            my_uart_send("CMD:STOP_MUSIC\r\n");
            play_state = 0;
            lv_timer_pause(progress_timer);
            
            // 隐藏进度条，光环变灰归位
            lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(playlist_time_label, LV_OBJ_FLAG_HIDDEN);
            if (deco_arc != NULL) lv_obj_set_style_arc_color(deco_arc, lv_color_hex(0x444444), LV_PART_INDICATOR);
            
            lv_label_set_text(label_status, "#00FF00 已停止 | 左滑退出#");
        } else {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU);
        }
        return;
    }

    if (play_state == 0) {
        if (cmd == UI_CMD_UP) {
            uint16_t sel = lv_roller_get_selected(roller_playlist);
            if (sel > 0) lv_roller_set_selected(roller_playlist, sel - 1, LV_ANIM_ON);
        }
        else if (cmd == UI_CMD_DOWN) {
            uint16_t sel = lv_roller_get_selected(roller_playlist);
            uint16_t count = lv_roller_get_option_cnt(roller_playlist);
            if (sel < count - 1) lv_roller_set_selected(roller_playlist, sel + 1, LV_ANIM_ON);
        }
        else if (cmd == UI_CMD_RIGHT) {
            char selected_file[32];
            lv_roller_get_selected_str(roller_playlist, selected_file, sizeof(selected_file));
            if (strcmp(selected_file, "暂无录音") != 0) {
                char cmd_buf[64];
                snprintf(cmd_buf, sizeof(cmd_buf), "CMD:PLAY_REC:%s\r\n", selected_file);
                my_uart_send(cmd_buf);
                lv_label_set_text(label_status, "#00FFFF 缓冲中...#");
            }
        }
    }
    else {
        if (cmd == UI_CMD_UP || cmd == UI_CMD_DOWN) {
            int step = (cmd == UI_CMD_UP) ? -5 : 5;
            current_sec += step;
            if (current_sec < 0) current_sec = 0;
            if (current_sec > total_sec) current_sec = total_sec;

            update_time_label();

            char cmd_buf[64];
            snprintf(cmd_buf, sizeof(cmd_buf), "CMD:SEEK_MUSIC:%d\r\n", current_sec);
            my_uart_send(cmd_buf);
        }
        else if (cmd == UI_CMD_RIGHT) {
            if (play_state == 1) {
                // 触发暂停
                my_uart_send("CMD:PAUSE_MUSIC\r\n");
                play_state = 2;
                lv_timer_pause(progress_timer);
                
                // UI 状态切为：暂停 (橙色主题)
                if (deco_arc != NULL) lv_obj_set_style_arc_color(deco_arc, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
                lv_label_set_text(label_status, "#FF8800 ⏸ 已暂停 | 右滑继续#");

            } else if (play_state == 2) {
                // 触发恢复
                my_uart_send("CMD:RESUME_MUSIC\r\n");
                play_state = 1;
                lv_timer_resume(progress_timer);

                // UI 状态恢复为：播放 (青色主题)
                if (deco_arc != NULL) lv_obj_set_style_arc_color(deco_arc, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
                lv_label_set_text(label_status, "#00FFFF ▶ 正在播放 | 右滑暂停#");
            }
        }
    }
}

// ==========================================
// 🎨 初始化界面
// ==========================================
void ui_playlist_screen_init(void) {
    ui_playlist_screen = lv_obj_create(NULL);
    // 纯黑背景与无边框
    lv_obj_set_style_bg_color(ui_playlist_screen, lv_color_black(), 0);
    lv_obj_set_style_border_width(ui_playlist_screen, 0, 0);

    lv_obj_t * title = lv_label_create(ui_playlist_screen);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "音频数据库");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 简化版：去掉装饰 arc，避免渲染卡顿
    deco_arc = NULL;

    // 透明滚轮设计
    roller_playlist = lv_roller_create(ui_playlist_screen);
    lv_obj_set_width(roller_playlist, 200);
    lv_roller_set_visible_row_count(roller_playlist, 5);
    lv_roller_set_options(roller_playlist, "获取中...", LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_bg_opa(roller_playlist, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller_playlist, 0, 0);
    lv_obj_set_style_text_color(roller_playlist, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(roller_playlist, lv_color_hex(0x00FFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller_playlist, LV_OPA_TRANSP, LV_PART_SELECTED);
    lv_obj_align(roller_playlist, LV_ALIGN_CENTER, 0, -15);

    // 进度条
    bar_progress = lv_bar_create(ui_playlist_screen);
    lv_obj_set_size(bar_progress, 200, 4);
    lv_obj_align(bar_progress, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
    lv_obj_add_flag(bar_progress, LV_OBJ_FLAG_HIDDEN);

    playlist_time_label = lv_label_create(ui_playlist_screen);
    lv_obj_set_style_text_font(playlist_time_label, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(playlist_time_label, lv_color_white(), 0);
    lv_label_set_text(playlist_time_label, "00:00 / 00:00");
    lv_obj_align(playlist_time_label, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_add_flag(playlist_time_label, LV_OBJ_FLAG_HIDDEN);

    label_status = lv_label_create(ui_playlist_screen);
    lv_label_set_recolor(label_status, true);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_label_set_text(label_status, "请求列表中...");
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -5);

    progress_timer = lv_timer_create(progress_timer_cb, 1000, NULL);
    lv_timer_pause(progress_timer);
}