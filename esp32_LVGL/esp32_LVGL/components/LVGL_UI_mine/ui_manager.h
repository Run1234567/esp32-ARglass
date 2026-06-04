#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "ui_globals.h"
#include "lvgl.h"

// ==========================================
//   J.A.R.V.I.S. AR眼镜系统全功能屏幕状态机声明
// ==========================================
typedef enum {
    SCREEN_MAIN_AR,     // 1. AR主视界 / 待机表盘
    SCREEN_MENU,        // 2. 滚动图标式主菜单
    SCREEN_CLOCK,       // 3. 翻页时钟/秒表小工具
    SCREEN_RECORD,      // 4. 录音机界面
    SCREEN_PLAYLIST,    // 5. 录音回放列表界面
    SCREEN_CAMERA,      // 6. 目标追踪/全息相机界面
    SCREEN_MUSIC,       // 7. 骨传导音乐播放器控制
    SCREEN_PITCH,       // 8. IMU姿态仪/魔法棒校准
    SCREEN_NOISE,       // 9. 白噪音深度睡眠专注模式
    SCREEN_NOVEL,       // 10. AI小说/AI智能对话交互
    SCREEN_GAME_LIST,   // 11. 游戏中心子菜单列表
    SCREEN_GAME,        // 12. 赛博跑酷
    SCREEN_GAME_2048    // 13. 经典体感 2048
} ui_screen_state_t;

// 外部引擎函数声明
void switch_to_screen(ui_screen_state_t target_screen);
void ui_manager_init(void);

// 命令队列 (供 BLE/UART 等模块发送指令)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
extern QueueHandle_t ui_cmd_queue;

#endif // UI_MANAGER_H