#ifndef _UI_MANAGER_H
#define _UI_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 1. 定义魔杖发来的动作指令 (匹配你的 AI 识别结果)
typedef enum {
    UI_CMD_NONE = 0,
    UI_CMD_UP,      // 上滑
    UI_CMD_DOWN,    // 下滑
    UI_CMD_LEFT,    // 左滑 (通常用作：返回/退出)
    UI_CMD_RIGHT    // 右滑 (通常用作：确认/进入)
} ui_cmd_t;

// 2. 定义系统中的所有屏幕状态
typedef enum {
    SCREEN_MAIN_AR, // AR 主界面 (时间/天气/电量)
    SCREEN_MENU,    // 主菜单 (滚轮)
    SCREEN_NOVEL,   // 小说阅读器
    SCREEN_CLOCK,   // ? 新增：时钟屏幕状态
    SCREEN_AI_CHAT  // AI 对话 (预留给你未来的功能)
} ui_screen_state_t;

// 3. 暴露全局消息队列句柄给蓝牙接收端 (my_ble.c) 和语音端 (main.c)
extern QueueHandle_t ui_cmd_queue;

// 4. 初始化 UI 系统并启动大管家任务
void ui_manager_init(void);

// ? 新增：暴露屏幕切换函数，方便 ui_clock_screen.c 异步退回
void switch_to_screen(ui_screen_state_t target_screen);

#endif // _UI_MANAGER_H
