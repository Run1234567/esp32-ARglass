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
#include "max30102.h"          // MAX30102 心率传感器
#include "my_uart.h"           // UART 串口通信模块

// ---- 游戏模块的外部函数声明 ----
// （游戏模块没有独立的 .h 文件，所以在这里用 extern 声明）
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

static const char *TAG = "UI_MANAGER"; // ESP_LOG 日志标签

// ============================================================
//   全局状态变量
// ============================================================
QueueHandle_t ui_cmd_queue = NULL;                    // 手势命令消息队列
static ui_screen_state_t current_screen = SCREEN_MAIN_AR; // 当前活跃屏幕状态

// ============================================================
//   屏幕切换引擎 —— 状态机核心函数
// ============================================================
// 工作流程：
//   1. 检查是否切到同一屏幕（避免重复加载）
//   2. 根据目标屏幕枚举，查找对应的 lv_obj_t* 屏幕对象
//   3. 处理切出旧屏幕时的清理工作（关闭麦克风、暂停定时器等）
//   4. 执行 LVGL 屏幕切换动画
//   5. 处理切入新屏幕时的初始化工作（开启麦克风、请求数据等）
void switch_to_screen(ui_screen_state_t target_screen) {
    // 如果目标屏幕就是当前屏幕，直接返回，避免重复切换
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
        case SCREEN_LIGHT:       target_obj = ui_light_screen;     break;
        case SCREEN_HEALTH:      target_obj = ui_health_screen;    break;
        default: return; // 未知屏幕枚举，直接返回
    }

    // ---- 第二步：切入新屏幕时，发送数据请求指令 ----
    // 录音回放列表：需要从外设主板获取录音文件列表
    if (target_screen == SCREEN_PLAYLIST) {
        my_uart_send("CMD:GET_REC_LIST\r\n");
    }
    // 音乐播放器：需要从外设主板获取音乐文件列表
    if (target_screen == SCREEN_MUSIC) {
        my_uart_send("CMD:GET_MUSIC_LIST\r\n");
    }
    // 小说阅读器：需要从外设主板获取书籍列表
    if (target_screen == SCREEN_NOVEL) {
        my_uart_send("CMD:GET_BOOKS\r\n");
    }

    // ---- 第三步：切出旧屏幕时，关闭不需要的硬件资源 ----
    // 噪声监测 / 声控游戏切出时：关闭麦克风采样
    // （这两个屏幕共享麦克风资源，只要其中一个激活就需要麦克风）
    if ((current_screen == SCREEN_NOISE || current_screen == SCREEN_GAME_NOTE) &&
        (target_screen != SCREEN_NOISE && target_screen != SCREEN_GAME_NOTE)) {
        my_uart_send("CMD:NOISE_OFF\r\n");
    }
    // 音高检测切出时：关闭音频 FFT 分析
    if (current_screen == SCREEN_PITCH && target_screen != SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_OFF\r\n");
    }
    // 像素鸟游戏切出时：暂停物理引擎定时器，防止后台继续运算
    if (current_screen == SCREEN_GAME_FLAPPY && target_screen != SCREEN_GAME_FLAPPY) {
        game_flappy_pause_timer();
    }
    if (current_screen == SCREEN_GAME_TETRIS && target_screen != SCREEN_GAME_TETRIS) {
        game_tetris_pause_timer();
    }
    if (current_screen == SCREEN_HEALTH && target_screen != SCREEN_HEALTH) {
        max30102_stop_task(); // 退出健康界面，停止心率采集
    }

    // ---- 第四步：执行 LVGL 屏幕切换 ----
    // LV_SCR_LOAD_ANIM_NONE 表示无动画、瞬间切换（适合 AR 眼镜的低延迟需求）
    lv_scr_load_anim(target_obj, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    current_screen = target_screen; // 更新当前屏幕状态

    // ---- 第五步：切入新屏幕时，开启所需的硬件资源 ----
    // 噪声监测 / 声控游戏切入时：开启麦克风采样
    if (current_screen == SCREEN_NOISE || current_screen == SCREEN_GAME_NOTE) {
        my_uart_send("CMD:NOISE_ON\r\n");
    }
    if (current_screen == SCREEN_HEALTH) {
        max30102_start_task(); // 进入健康界面，启动心率采集
    }
    // 音高检测切入时：开启音频 FFT 分析
    if (current_screen == SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_ON\r\n");
    }

    ESP_LOGI(TAG, "Screen switched to: %d", current_screen);
}

// ============================================================
//   手势指令分发器 —— 状态机路由表
// ============================================================
// 根据 current_screen 的值，将同一个手势指令分发到不同的处理函数。
// 例如：同样是 UI_CMD_UP，在菜单界面是「上移选中项」，
//       在音乐播放器是「增大音量」，在小说阅读器是「上翻一行」。
static void process_ui_command(ui_cmd_t cmd) {
    switch (current_screen) {

        // ---- AR 主视界：只有右滑能进入菜单 ----
        case SCREEN_MAIN_AR:
            if (cmd == UI_CMD_RIGHT) {
                switch_to_screen(SCREEN_MENU);
            }
            break;

        // ---- 主菜单：上下滚动选择，左滑返回，右滑确认进入 ----
        case SCREEN_MENU:
            if (cmd == UI_CMD_UP) {
                menu_scroll_up();
            }
            else if (cmd == UI_CMD_DOWN) {
                menu_scroll_down();
            }
            else if (cmd == UI_CMD_LEFT) {
                switch_to_screen(SCREEN_MAIN_AR);
            }
            else if (cmd == UI_CMD_RIGHT) {
                // 根据滚轮选中项的索引，跳转到对应屏幕
                uint16_t selected_idx = lv_roller_get_selected(menu_roller);
                if (selected_idx == 0)  switch_to_screen(SCREEN_MAIN_AR);
                if (selected_idx == 1)  switch_to_screen(SCREEN_HEALTH);
                if (selected_idx == 2)  switch_to_screen(SCREEN_CLOCK);
                if (selected_idx == 3)  switch_to_screen(SCREEN_RECORD);
                if (selected_idx == 4)  switch_to_screen(SCREEN_PLAYLIST);
                if (selected_idx == 5)  switch_to_screen(SCREEN_CAMERA);
                if (selected_idx == 7)  switch_to_screen(SCREEN_MUSIC);
                if (selected_idx == 8)  switch_to_screen(SCREEN_PITCH);
                if (selected_idx == 9)  switch_to_screen(SCREEN_NOISE);
                if (selected_idx == 10) switch_to_screen(SCREEN_NOVEL);
                if (selected_idx == 11) switch_to_screen(SCREEN_GAME_LIST);
                if (selected_idx == 12) switch_to_screen(SCREEN_LIGHT);
            }
            break;

        // ---- 以下屏幕将手势委托给各自的处理函数 ----
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

        // ---- 噪声监测 / 音高检测：左滑返回菜单 ----
        case SCREEN_NOISE:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            break;

        case SCREEN_PITCH:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            break;

        case SCREEN_MUSIC:
            music_screen_handle_cmd(cmd);
            break;

        // ---- 游戏模块：委托给各自的手势处理函数 ----
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

        case SCREEN_LIGHT:
            light_screen_handle_cmd(cmd);
            break;

        case SCREEN_HEALTH:
            health_screen_handle_cmd(cmd);
            break;

        default:
            break;
    }
}

// ============================================================
//   UI 守护任务 —— FreeRTOS 后台任务
// ============================================================
// 这是一个独立运行的 FreeRTOS 任务，核心职责是：
//   1. 无限循环监听手势命令队列（阻塞等待，不消耗 CPU）
//   2. 收到命令后，获取 LVGL 线程锁（保证 UI 操作的线程安全）
//   3. 调用 process_ui_command() 执行手势分发
//   4. 释放 LVGL 线程锁
//
// 优先级为 5，绑定到 CPU 核心 1（与 LVGL 渲染任务同核，避免跨核竞争）
static void ui_manager_task(void *pvParameter) {
    ui_cmd_t received_cmd;

    while (1) {
        // 阻塞等待队列中的手势命令（portMAX_DELAY = 永久等待）
        if (xQueueReceive(ui_cmd_queue, &received_cmd, portMAX_DELAY) == pdTRUE) {
            // 获取 LVGL 线程锁（超时 0 = 立即获取，失败则跳过）
            if (lvgl_port_lock(0)) {
                process_ui_command(received_cmd); // 执行手势分发
                lvgl_port_unlock();               // 释放 LVGL 线程锁
            }
        }
    }
}

// ============================================================
//   UI 管理器初始化 —— 系统启动时调用一次
// ============================================================
// 初始化顺序：
//   1. 依次初始化所有屏幕模块（每个模块会创建自己的 UI 对象）
//   2. 加载主屏幕作为初始画面
//   3. 创建手势命令消息队列（容量 10 条）
//   4. 启动 UI 守护任务
void ui_manager_init(void) {
    // ---- 第一步：初始化所有屏幕（在 LVGL 锁保护下执行） ----
    if (lvgl_port_lock(0)) {
        ui_ar_glass_init();          // AR 主视界
        ui_menu_screen_init();       // 主菜单
        ui_novel_screen_init();      // 小说阅读器
        ui_clock_screen_init();      // 翻页时钟
        ui_record_screen_init();     // 录音机
        ui_playlist_screen_init();   // 录音回放列表
        ui_camera_screen_init();     // 相机
        ui_noise_screen_init();      // 噪声监测
        ui_pitch_screen_init();      // 音高检测
        ui_music_screen_init();      // 音乐播放器
        ui_game_list_screen_init();  // 游戏中心列表
        ui_game_screen_init();       // 赛博跑酷
        ui_game_2048_init();         // 经典 2048
        ui_game_flappy_init();       // 像素鸟
        ui_game_note_init();         // 声控八分音符酱
        ui_game_tetris_init();       // 俄罗斯方块
        ui_light_screen_init();      // 光照传感器界面
        ui_health_screen_init();     // 心率血氧监测

        // 加载主屏幕作为开机初始画面
        lv_scr_load(ui_main_screen);
        lvgl_port_unlock();
    }

    // ---- 第二步：创建手势命令消息队列 ----
    // 队列容量 10 条，每条大小为 sizeof(ui_cmd_t)
    ui_cmd_queue = xQueueCreate(10, sizeof(ui_cmd_t));

    // ---- 第三步：启动 UI 守护任务 ----
    // 栈大小 4KB，优先级 5，绑定到 CPU 核心 1
    xTaskCreatePinnedToCore(ui_manager_task, "ui_mgr", 1024 * 4, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "UI Manager initialized!");
}
