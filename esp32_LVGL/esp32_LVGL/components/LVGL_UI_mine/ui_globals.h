// ============================================================
// ui_globals.h
// J.A.R.V.I.S. AR 智能眼镜 —— 全局变量与类型声明头文件
// ============================================================
// 作用：集中声明所有 UI 屏幕对象的全局指针、共享数据变量、
//       以及手势指令枚举类型。所有需要跨模块访问的 UI 对象
//       都在这里用 extern 声明，实际定义在各自的 .c 文件中。
//
// 使用方式：任何需要访问全局屏幕对象或共享数据的 .c 文件，
//           只需 #include "ui_globals.h" 即可。
// ============================================================

#ifndef _UI_GLOBALS_H
#define _UI_GLOBALS_H

#include "lvgl.h"  // 引入 LVGL 图形库核心头文件

// ============================================================
//   手势/指令枚举类型定义
// ============================================================
// 这个枚举定义了魔杖（IMU 体感控制器）能识别的所有手势方向。
// 通过 BLE 蓝牙或 UART 串口传入 UI 管理器的消息队列，
// 再由 ui_manager.c 的 process_ui_command() 分发到当前屏幕。
typedef enum {
    UI_CMD_NONE = 0,   // 无操作（空指令）
    UI_CMD_UP,         // 向上挥动魔杖
    UI_CMD_DOWN,       // 向下挥动魔杖
    UI_CMD_LEFT,       // 向左挥动魔杖
    UI_CMD_RIGHT,      // 向右挥动魔杖
    UI_CMD_CENTER,     // 按下魔杖中心按钮
    UI_CMD_CIRCLE,     // 画圈手势
    UI_CMD_FORWARD,    // 向前靠近
    UI_CMD_BACKWARD,   // 向后远离
    UI_CMD_WAVE        // 快速挥手
} ui_cmd_t;

// ============================================================
//   自定义中文字体声明
// ============================================================
// 这是一个 16px 的中文字体，由 LVGL Font Converter 工具生成，
// 包含了常用中文字符和 Emoji 表情。所有需要显示中文的 UI
// 组件都必须调用 lv_obj_set_style_text_font() 设置此字体。
LV_FONT_DECLARE(my_font_cn_16);

// ============================================================
//   全局屏幕对象指针声明
// ============================================================
// 每个屏幕模块（如 ui_clock_screen.c）内部定义一个 lv_obj_t*，
// 这里用 extern 声明让其他模块（尤其是 ui_manager.c）可以访问。
// ui_manager.c 的 switch_to_screen() 函数通过这些指针来切换屏幕。
extern lv_obj_t * ui_main_screen;      // AR 主视界 / 待机表盘
extern lv_obj_t * ui_menu_screen;      // 滚动图标式主菜单
extern lv_obj_t * ui_clock_screen;     // 翻页时钟 / 秒表小工具
extern lv_obj_t * ui_playlist_screen;  // 录音回放列表界面
extern lv_obj_t * ui_novel_screen;     // AI 小说 / 智能对话交互
extern lv_obj_t * ui_game_list_screen; // 游戏中心子菜单列表
extern lv_obj_t * ui_game_2048_screen; // 经典体感 2048 游戏
extern lv_obj_t * ui_game_screen;      // 赛博跑酷游戏
extern lv_obj_t * ui_game_flappy_screen; // 像素鸟游戏
extern lv_obj_t * ui_game_note_screen;   // 声控八分音符酱游戏
extern lv_obj_t * ui_game_tetris_screen; // 俄罗斯方块
extern lv_obj_t * ui_game_mole_screen;   // 打地鼠
extern lv_obj_t * ui_game_snake_screen;  // 贪吃蛇
extern lv_obj_t * ui_game_rhythm_screen; // 节奏魔杖
extern lv_obj_t * ui_game_simon_screen;  // 记忆大师
extern lv_obj_t * ui_light_screen;       // 光照传感器界面
extern lv_obj_t * ui_health_screen;      // 心率血氧监测
extern lv_obj_t * ui_wifi_scan_screen;   // Wi-Fi 扫描界面
extern lv_obj_t * ui_gps_screen;         // GPS 定位界面
extern lv_obj_t * ui_ai_screen;          // AI 字幕界面

// ============================================================
//   MQTT 数据 / 时钟模块共享变量
// ============================================================
// 这些变量由 MQTT 模块（app_mqtt.c）通过回调写入，
// 由 ui_clock_screen.c 读取并刷新到屏幕上。
// 实现了「云端天气数据 → 屏幕显示」的数据流。
extern lv_obj_t * label_time;      // 时间标签（HH:MM:SS）
extern lv_obj_t * label_date;      // 日期标签（YYYY-MM-DD）
extern lv_obj_t * label_lunar;     // 农历日期标签
extern lv_obj_t * label_weather;   // 天气信息标签
extern lv_obj_t * label_batt_pct;  // 电池电量百分比标签
extern lv_obj_t * icon_batt;       // 电池图标
extern lv_obj_t * menu_roller;     // 主菜单滚轮选择器

// ============================================================
//   小说模块共享变量
// ============================================================
// 小说模块通过 UART 串口从外设主板获取书籍列表和章节内容，
// 存储在 novel_source_buffer 中，由 LVGL 定时器逐行滚动显示。
extern lv_obj_t * label_novel_text;     // 小说正文显示标签
extern lv_obj_t * novel_scroll_cont;    // 小说滚动容器
extern const char * test_novel_text;    // 内置测试小说文本
extern uint8_t novel_scroll_task_running; // 自动滚动任务运行标志
extern char * novel_source_buffer;       // 小说文本数据缓冲区
extern size_t current_book_pos;          // 当前阅读位置（字节偏移）

// ============================================================
//   音乐模块共享变量
// ============================================================
// 音乐播放器的状态数据，由 UART 串口回调更新，
// 由 ui_music_screen.c 读取并刷新进度条和歌词。
extern int music_total_time;           // 歌曲总时长（秒）
extern int music_current_time;         // 当前播放进度（秒）
extern char music_current_song[64];    // 当前歌曲名称

#endif // _UI_GLOBALS_H
