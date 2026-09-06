// ============================================================
// ui_manager.c
// J.A.R.V.I.S. AR 智能眼镜 —— UI 管理器核心实现
// ============================================================
// 这是整个 UI 系统的「大脑」，负责三大核心职责：
//
//   1. 屏幕切换引擎（switch_to_screen）
//      - 管理当前活跃屏幕状态
//      - 处理屏幕切入/切出时的副作用（麦克风开关、定时器暂停等）
//      - 调用 LVGL 的 lv_scr_load_anim() 执行画面切换
//
//   2. 手势指令分发器（process_ui_command）
//      - 根据 current_screen 状态，将手势指令路由到对应的屏幕处理函数
//      - 实现了「同一手势在不同屏幕有不同行为」的状态机逻辑
//
//   3. UI 守护任务（ui_manager_task）
//      - 一个 FreeRTOS 任务，持续监听手势命令队列
//      - 收到命令后获取 LVGL 锁，执行指令分发，释放锁
//      - 保证 UI 操作的线程安全性
// ============================================================

#include "ui_manager.h"
#include "ui_globals.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

// ---- 引入所有屏幕模块的头文件 ----
#include "ui_ar_glass.h"       // AR 主视界
#include "ui_menu_screen.h"    // 主菜单
#include "ui_novel_screen.h"   // 小说阅读器
#include "ui_clock_screen.h"   // 翻页时钟
#include "ui_record_screen.h"  // 录音机
#include "ui_playlist_screen.h"// 录音回放列表
#include "ui_camera_screen.h"  // 相机
#include "ui_noise_screen.h"   // 噪声监测 / 白噪音
#include "ui_pitch_screen.h"   // 音高检测
#include "ui_music_screen.h"   // 音乐播放器
#include "ui_light_screen.h"   // 光照传感器界面
#include "ui_game_tetris.h"    // 俄罗斯方块
#include "ui_health_screen.h"  // 心率血氧监测
#include "ui_game_mole.h"      // 打地鼠
#include "ui_game_snake.h"     // 贪吃蛇
#include "ui_game_rhythm.h"    // 节奏魔杖
#include "ui_game_simon.h"     // 记忆大师
#include "ui_wifi_scan_screen.h" // Wi-Fi 扫描
#include "ui_gps_screen.h"       // GPS 定位
#include "ui_ai_screen.h"       // AI 字幕
#include "ui_call_screen.h"    // 网络通话
#include "ui_audio_switch_screen.h" // 音频切换
#include "ui_video_screen.h"       // AR 录像机
#include "ui_translate_screen.h"   // 翻译模式
#include "ui_translate_lang_screen.h" // 翻译语言选择
#include "ui_translate_mode_screen.h" // 翻译模式选择
#include "ui_step_screen.h"        // 计步器
#include "max30102.h"          // MAX30102 心率传感器
#include "my_uart.h"           // UART 串口通信模块

// ---- 游戏模块的外部函数声明 ----
extern void ui_game_screen_init(void);           // 赛博跑酷初始化
extern void game_screen_handle_cmd(ui_cmd_t cmd); // 赛博跑酷手势处理
extern void ui_game_2048_init(void);              // 2048 初始化
extern void game_2048_screen_handle_cmd(ui_cmd_t cmd); // 2048 手势处理
extern void ui_game_list_screen_init(void);       // 游戏列表初始化
extern void game_list_screen_handle_cmd(ui_cmd_t cmd); // 游戏列表手势处理
extern void ui_game_flappy_init(void);            // 像素鸟初始化
extern void game_flappy_screen_handle_cmd(ui_cmd_t cmd); // 像素鸟手势处理
extern void game_flappy_pause_timer(void);        // 像素鸟定时器安全暂停
extern void ui_game_note_init(void);              // 声控八分音符初始化
extern void game_note_screen_handle_cmd(ui_cmd_t cmd); // 声控八分音符手势处理
extern void game_tetris_pause_timer(void);        // 俄罗斯方块定时器暂停

// ---- 翻译模块的外部函数声明 ----
extern void ui_enter_translate_mode(void);
extern void ui_exit_translate_mode(void);

static const char *TAG = "UI_MANAGER"; // ESP_LOG 日志标签

// ============================================================
//   全局状态变量
// ============================================================
QueueHandle_t ui_cmd_queue = NULL;                    // 手势命令消息队列
static ui_screen_state_t current_screen = SCREEN_MAIN_AR; // 当前活跃屏幕状态

// ============================================================
//   屏幕切换引擎 —— 状态机核心函数
// ============================================================
void switch_to_screen(ui_screen_state_t target_screen) {
    if (current_screen == target_screen) return;

    lv_obj_t * target_obj = NULL;

    // ---- 第一步：根据枚举值查找目标屏幕对象 ----
    switch (target_screen) {
        case SCREEN_MAIN_AR:     target_obj = ui_main_screen;      break;
        case SCREEN_MENU:        target_obj = ui_menu_screen;      break;
        case SCREEN_NOVEL:       target_obj = ui_novel_screen;     break;
        case SCREEN_CLOCK:       target_obj = ui_clock_screen;     break;
        case SCREEN_RECORD:      target_obj = ui_record_screen;    break;
        case SCREEN_PLAYLIST:    target_obj = ui_playlist_screen;  break;
        case SCREEN_CAMERA:      target_obj = ui_camera_screen;    break;
        case SCREEN_NOISE:       target_obj = ui_noise_screen;     break;
        case SCREEN_PITCH:       target_obj = ui_pitch_screen;     break;
        case SCREEN_MUSIC:       target_obj = ui_music_screen;     break;
        case SCREEN_GAME_LIST:   target_obj = ui_game_list_screen; break;
        case SCREEN_GAME:        target_obj = ui_game_screen;      break;
        case SCREEN_GAME_2048:   target_obj = ui_game_2048_screen; break;
        case SCREEN_GAME_FLAPPY: target_obj = ui_game_flappy_screen; break;
        case SCREEN_GAME_NOTE:   target_obj = ui_game_note_screen; break;
        case SCREEN_GAME_TETRIS: target_obj = ui_game_tetris_screen; break;
        case SCREEN_GAME_MOLE:   target_obj = ui_game_mole_screen;  break;
        case SCREEN_GAME_SNAKE:  target_obj = ui_game_snake_screen; break;
        case SCREEN_GAME_RHYTHM: target_obj = ui_game_rhythm_screen; break;
        case SCREEN_GAME_SIMON:  target_obj = ui_game_simon_screen;  break;
        case SCREEN_WIFI_SCAN:   target_obj = ui_wifi_scan_screen;  break;
        case SCREEN_GPS:         target_obj = ui_gps_screen;        break;
        case SCREEN_AI_CHAT:     target_obj = ui_ai_screen;        break;
        case SCREEN_CALL:        target_obj = ui_call_screen;      break;
        case SCREEN_AUDIO_SWITCH: target_obj = ui_audio_switch_screen; break;
        case SCREEN_LIGHT:       target_obj = ui_light_screen;     break;
        case SCREEN_HEALTH:      target_obj = ui_health_screen;    break;
        case SCREEN_VIDEO:       target_obj = ui_video_screen;     break;
        case SCREEN_TRANSLATE_MODE: target_obj = ui_translate_mode_screen; break;
        case SCREEN_TRANSLATE_LANG: target_obj = ui_translate_lang_screen; break;
        case SCREEN_TRANSLATE:   target_obj = ui_translate_screen; break;
        case SCREEN_STEP:        target_obj = ui_step_screen; break;
        default: return; 
    }

    // ---- 第二步：切入新屏幕时，发送数据请求指令 ----
    if (target_screen == SCREEN_PLAYLIST)  my_uart_send("CMD:GET_REC_LIST\r\n");
    if (target_screen == SCREEN_MUSIC)     my_uart_send("CMD:GET_MUSIC_LIST\r\n");
    if (target_screen == SCREEN_NOVEL)     my_uart_send("CMD:GET_BOOKS\r\n");

    // ---- 第三步：切出旧屏幕时，关闭不需要的硬件资源 ----
    if ((current_screen == SCREEN_NOISE || current_screen == SCREEN_GAME_NOTE) &&
        (target_screen != SCREEN_NOISE && target_screen != SCREEN_GAME_NOTE)) {
        my_uart_send("CMD:NOISE_OFF\r\n");
    }
    if (current_screen == SCREEN_PITCH && target_screen != SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_OFF\r\n");
    }
    if (current_screen == SCREEN_GAME_FLAPPY && target_screen != SCREEN_GAME_FLAPPY) {
        game_flappy_pause_timer();
    }
    if (current_screen == SCREEN_GAME_TETRIS && target_screen != SCREEN_GAME_TETRIS) {
        game_tetris_pause_timer();
    }
    if (current_screen == SCREEN_GAME_MOLE && target_screen != SCREEN_GAME_MOLE) {
        game_mole_pause_timer();
    }
    if (current_screen == SCREEN_GAME_SNAKE && target_screen != SCREEN_GAME_SNAKE) {
        game_snake_pause_timer();
    }
    if (current_screen == SCREEN_GAME_RHYTHM && target_screen != SCREEN_GAME_RHYTHM) {
        game_rhythm_pause_timer();
    }
    if (current_screen == SCREEN_GAME_SIMON && target_screen != SCREEN_GAME_SIMON) {
        game_simon_pause_timer();
    }
    if (current_screen == SCREEN_GPS && target_screen != SCREEN_GPS) {
        ui_gps_stop_update();
    }
    if (current_screen == SCREEN_CALL && target_screen != SCREEN_CALL) {
        extern volatile bool is_calling_now;
        is_calling_now = false;
    }
    if (current_screen == SCREEN_HEALTH && target_screen != SCREEN_HEALTH) {
        max30102_stop_task();
    }
    // 如果当前是翻译模式，且准备跳出，则停止翻译
    if (current_screen == SCREEN_TRANSLATE && target_screen != SCREEN_TRANSLATE) {
        ui_exit_translate_mode();
    }

    // ---- 第四步：执行 LVGL 屏幕切换 ----
    lv_scr_load_anim(target_obj, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    current_screen = target_screen; 

    // ---- 第五步：切入新屏幕时，开启所需的硬件资源 ----
    if (current_screen == SCREEN_NOISE || current_screen == SCREEN_GAME_NOTE) {
        my_uart_send("CMD:NOISE_ON\r\n");
    }
    if (current_screen == SCREEN_HEALTH) {
        max30102_start_task();
    }
    if (current_screen == SCREEN_WIFI_SCAN) {
        ui_wifi_scan_start();
    }
    if (current_screen == SCREEN_GPS) {
        ui_gps_start_update();
    }
    if (current_screen == SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_ON\r\n");
    }
    // 如果目标屏幕是翻译模式，则启动翻译
    if (current_screen == SCREEN_TRANSLATE) {
        ui_enter_translate_mode();
    }

    ESP_LOGI(TAG, "Screen switched to: %d", current_screen);
}

// ============================================================
//   手势指令分发器 —— 状态机路由表 (已加入画圈动作)
// ============================================================
static void process_ui_command(ui_cmd_t cmd) {
    // 全局屏幕跳转（MQTT/网页远程控制）
    switch (cmd) {
        case UI_CMD_GOTO_AR:        switch_to_screen(SCREEN_MAIN_AR); return;
        case UI_CMD_GOTO_MENU:      switch_to_screen(SCREEN_MENU); return;
        case UI_CMD_GOTO_NOVEL:     switch_to_screen(SCREEN_NOVEL); return;
        case UI_CMD_GOTO_CLOCK:     switch_to_screen(SCREEN_CLOCK); return;
        case UI_CMD_GOTO_RECORD:    switch_to_screen(SCREEN_RECORD); return;
        case UI_CMD_GOTO_PLAYLIST:  switch_to_screen(SCREEN_PLAYLIST); return;
        case UI_CMD_GOTO_CAMERA:    switch_to_screen(SCREEN_CAMERA); return;
        case UI_CMD_GOTO_NOISE:     switch_to_screen(SCREEN_NOISE); return;
        case UI_CMD_GOTO_PITCH:     switch_to_screen(SCREEN_PITCH); return;
        case UI_CMD_GOTO_MUSIC:     switch_to_screen(SCREEN_MUSIC); return;
        case UI_CMD_GOTO_LIGHT:     switch_to_screen(SCREEN_LIGHT); return;
        case UI_CMD_GOTO_HEALTH:    switch_to_screen(SCREEN_HEALTH); return;
        case UI_CMD_GOTO_WIFI:      switch_to_screen(SCREEN_WIFI_SCAN); return;
        case UI_CMD_GOTO_GPS:       switch_to_screen(SCREEN_GPS); return;
        case UI_CMD_GOTO_AI:        switch_to_screen(SCREEN_AI_CHAT); return;
        case UI_CMD_GOTO_CALL:      switch_to_screen(SCREEN_CALL); return;
        case UI_CMD_GOTO_AUDIO:     switch_to_screen(SCREEN_AUDIO_SWITCH); return;
        case UI_CMD_GOTO_GAME_LIST: switch_to_screen(SCREEN_GAME_LIST); return;
        case UI_CMD_GOTO_TRANSLATE: switch_to_screen(SCREEN_TRANSLATE); return;
        case UI_CMD_GOTO_TRANSLATE_LANG: switch_to_screen(SCREEN_TRANSLATE_LANG); return;
        case UI_CMD_GOTO_TRANSLATE_MODE: switch_to_screen(SCREEN_TRANSLATE_MODE); return;
        default: break;
    }

    switch (current_screen) {

        // ---- AR 主视界 ----
        case SCREEN_MAIN_AR:
            if (cmd == UI_CMD_RIGHT) {
                switch_to_screen(SCREEN_MENU);
            } 
            // 【修改点 1】：在主页面画个圈，直接盲操唤醒相机拍照！
            else if (cmd == UI_CMD_CIRCLE) { 
                switch_to_screen(SCREEN_CAMERA);
            }
            break;

        // ---- 主菜单（支持魔杖 + PAJ7620 手势） ----
        case SCREEN_MENU:
            if (cmd == UI_CMD_UP) {
                uint16_t before = lv_roller_get_selected(menu_roller);
                menu_scroll_up();
                uint16_t after = lv_roller_get_selected(menu_roller);
                ESP_LOGI("MENU", "上滑: %d -> %d", before, after);
            }
            else if (cmd == UI_CMD_DOWN) {
                menu_scroll_down();
            }
            else if (cmd == UI_CMD_LEFT) {
                switch_to_screen(SCREEN_MAIN_AR);
            }
            else if (cmd == UI_CMD_CIRCLE) {
                switch_to_screen(SCREEN_MAIN_AR);
            }
            else if (cmd == UI_CMD_RIGHT) {
                uint16_t selected_idx = lv_roller_get_selected(menu_roller);
                if (selected_idx == 0)  switch_to_screen(SCREEN_MAIN_AR);
                if (selected_idx == 1)  switch_to_screen(SCREEN_HEALTH);
                if (selected_idx == 2)  switch_to_screen(SCREEN_CLOCK);
                if (selected_idx == 3)  switch_to_screen(SCREEN_RECORD);
                if (selected_idx == 4)  switch_to_screen(SCREEN_PLAYLIST);
                if (selected_idx == 5)  switch_to_screen(SCREEN_CAMERA);
                if (selected_idx == 6)  switch_to_screen(SCREEN_AI_CHAT);
                if (selected_idx == 7)  switch_to_screen(SCREEN_MUSIC);
                if (selected_idx == 8)  switch_to_screen(SCREEN_PITCH);
                if (selected_idx == 9)  switch_to_screen(SCREEN_NOISE);
                if (selected_idx == 10) switch_to_screen(SCREEN_NOVEL);
                if (selected_idx == 11) switch_to_screen(SCREEN_GAME_LIST);
                if (selected_idx == 12) switch_to_screen(SCREEN_LIGHT);
                if (selected_idx == 13) switch_to_screen(SCREEN_WIFI_SCAN);
                if (selected_idx == 14) switch_to_screen(SCREEN_GPS);
                if (selected_idx == 15) switch_to_screen(SCREEN_CALL);
                if (selected_idx == 16) switch_to_screen(SCREEN_AUDIO_SWITCH);
                if (selected_idx == 17) switch_to_screen(SCREEN_VIDEO);
                if (selected_idx == 18) switch_to_screen(SCREEN_TRANSLATE_MODE);
                if (selected_idx == 19) switch_to_screen(SCREEN_STEP);
            }
            break;

        // ---- 以下屏幕直接透传给各自模块（它们会自动在各自的内部收到 UI_CMD_CIRCLE） ----
        case SCREEN_NOVEL:
            novel_screen_handle_cmd(cmd);
            break;

        case SCREEN_CLOCK:
            clock_screen_handle_cmd(cmd);
            break;

        case SCREEN_RECORD:
            record_screen_handle_cmd(cmd);
            break;

        case SCREEN_PLAYLIST:
            playlist_screen_handle_cmd(cmd);
            break;

        case SCREEN_CAMERA:
            camera_screen_handle_cmd(cmd);
            break;

        // ---- 噪声监测 / 音高检测 ----
        case SCREEN_NOISE:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            // 【修改点 3】：一键回主界面
            else if (cmd == UI_CMD_CIRCLE) switch_to_screen(SCREEN_MAIN_AR); 
            break;

        case SCREEN_PITCH:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            // 【修改点 4】：一键回主界面
            else if (cmd == UI_CMD_CIRCLE) switch_to_screen(SCREEN_MAIN_AR); 
            break;

        case SCREEN_MUSIC:
            music_screen_handle_cmd(cmd);
            break;

        // ---- 游戏及其他扩展模块：直接透传 ----
        case SCREEN_GAME_LIST:
            game_list_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME:
            game_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_2048:
            game_2048_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_FLAPPY:
            game_flappy_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_NOTE:
            game_note_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_TETRIS:
            game_tetris_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_MOLE:
            game_mole_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_SNAKE:
            game_snake_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_RHYTHM:
            game_rhythm_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_SIMON:
            game_simon_screen_handle_cmd(cmd);
            break;

        case SCREEN_WIFI_SCAN:
            wifi_scan_screen_handle_cmd(cmd);
            break;

        case SCREEN_GPS:
            gps_screen_handle_cmd(cmd);
            break;

        case SCREEN_AI_CHAT:
            ai_screen_handle_cmd(cmd);
            break;

        case SCREEN_CALL:
            call_screen_handle_cmd(cmd);
            break;

        case SCREEN_AUDIO_SWITCH:
            audio_switch_handle_cmd(cmd);
            break;

        case SCREEN_LIGHT:
            light_screen_handle_cmd(cmd);
            break;

        case SCREEN_HEALTH:
            health_screen_handle_cmd(cmd);
            break;

        case SCREEN_VIDEO:
            video_screen_handle_cmd(cmd);
            break;

        case SCREEN_TRANSLATE:
            translate_screen_handle_cmd(cmd);
            break;

        case SCREEN_TRANSLATE_LANG:
            translate_lang_screen_handle_cmd(cmd);
            break;

        case SCREEN_TRANSLATE_MODE:
            translate_mode_screen_handle_cmd(cmd);
            break;

        case SCREEN_STEP:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            else if (cmd == UI_CMD_CIRCLE) switch_to_screen(SCREEN_MAIN_AR);
            break;

        default:
            break;
    }
}

// ============================================================
//   UI 守护任务 —— FreeRTOS 后台任务
// ============================================================
static void ui_manager_task(void *pvParameter) {
    ui_cmd_t received_cmd;

    while (1) {
        if (xQueueReceive(ui_cmd_queue, &received_cmd, portMAX_DELAY) == pdTRUE) {
            if (lvgl_port_lock(0)) {
                process_ui_command(received_cmd); 
                lvgl_port_unlock();               
            }
        }
    }
}

// ============================================================
//   UI 管理器初始化 —— 系统启动时调用一次
// ============================================================
void ui_manager_init(void) {
    ESP_LOGI(TAG, "尝试获取 LVGL 锁...");
    if (lvgl_port_lock(0)) {
        ESP_LOGI(TAG, "LVGL 锁获取成功，开始初始化 UI...");

        ESP_LOGI(TAG, "初始化 AR 主界面...");
        ui_ar_glass_init();
        ESP_LOGI(TAG, "AR 主界面初始化完成, ui_main_screen=%p", (void*)ui_main_screen);

        ESP_LOGI(TAG, "初始化菜单...");
        ui_menu_screen_init();
        ESP_LOGI(TAG, "初始化小说...");
        ui_novel_screen_init();
        ESP_LOGI(TAG, "初始化时钟...");
        ui_clock_screen_init();
        ESP_LOGI(TAG, "初始化录音...");
        ui_record_screen_init();
        ESP_LOGI(TAG, "初始化播放列表...");
        ui_playlist_screen_init();
        ESP_LOGI(TAG, "初始化相机...");
        ui_camera_screen_init();
        ESP_LOGI(TAG, "初始化噪声...");
        ui_noise_screen_init();
        ESP_LOGI(TAG, "初始化音高...");
        ui_pitch_screen_init();
        ESP_LOGI(TAG, "初始化音乐...");
        ui_music_screen_init();
        ESP_LOGI(TAG, "初始化游戏列表...");
        ui_game_list_screen_init();
        ESP_LOGI(TAG, "初始化赛博跑酷...");
        ui_game_screen_init();
        ESP_LOGI(TAG, "初始化 2048...");
        ui_game_2048_init();
        ESP_LOGI(TAG, "初始化像素鸟...");
        ui_game_flappy_init();
        ESP_LOGI(TAG, "初始化声控八分音符...");
        ui_game_note_init();
        ESP_LOGI(TAG, "初始化俄罗斯方块...");
        ui_game_tetris_init();
        ESP_LOGI(TAG, "初始化打地鼠...");
        ui_game_mole_init();
        ESP_LOGI(TAG, "初始化贪吃蛇...");
        ui_game_snake_init();
        ESP_LOGI(TAG, "初始化节奏魔杖...");
        ui_game_rhythm_init();
        ESP_LOGI(TAG, "初始化记忆大师...");
        ui_game_simon_init();
        ESP_LOGI(TAG, "初始化 WiFi 扫描...");
        ui_wifi_scan_screen_init();
        ESP_LOGI(TAG, "初始化 GPS...");
        ui_gps_screen_init();
        ESP_LOGI(TAG, "初始化 AI...");
        ui_ai_screen_init();
        ESP_LOGI(TAG, "初始化通话...");
        ui_call_screen_init();
        ESP_LOGI(TAG, "初始化音频切换...");
        ui_audio_switch_screen_init();
        ESP_LOGI(TAG, "初始化光照...");
        ui_light_screen_init();
        ESP_LOGI(TAG, "初始化健康...");
        ui_health_screen_init();
        ESP_LOGI(TAG, "初始化录像...");
        ui_video_screen_init();
        ESP_LOGI(TAG, "初始化翻译...");
        ui_translate_screen_init();
        ESP_LOGI(TAG, "初始化翻译语言选择...");
        ui_translate_lang_screen_init();
        ESP_LOGI(TAG, "初始化翻译模式选择...");
        ui_translate_mode_screen_init();
        ESP_LOGI(TAG, "初始化计步器...");
        ui_step_screen_init();

        ESP_LOGI(TAG, "加载主屏幕...");
        lv_scr_load(ui_main_screen);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "UI 初始化全部完成！");
    } else {
        ESP_LOGE(TAG, "获取 LVGL 锁失败！UI 未初始化");
    }

    ui_cmd_queue = xQueueCreate(10, sizeof(ui_cmd_t));
    xTaskCreatePinnedToCore(ui_manager_task, "ui_mgr", 1024 * 4, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "UI Manager initialized!");
}