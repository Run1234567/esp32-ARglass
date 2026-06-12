// ============================================================
// ui_manager.h
// J.A.R.V.I.S. AR 智能眼镜 —— UI 管理器头文件
// ============================================================
// 作用：定义屏幕状态机枚举（所有屏幕的唯一 ID），
//       声明 UI 管理器对外暴露的接口函数和命令队列。
//
// 核心设计思想：
//   整个 UI 系统采用「有限状态机」架构。任意时刻只有一个屏幕
//   处于激活状态（current_screen），通过 switch_to_screen()
//   函数进行切换。所有手势输入统一由 process_ui_command()
//   根据当前屏幕状态分发到对应的屏幕处理函数。
// ============================================================

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "ui_globals.h"
#include "lvgl.h"

// ============================================================
//   屏幕状态机枚举 —— 每个屏幕的唯一编号
// ============================================================
// 这个枚举定义了系统中所有的屏幕状态。
// switch_to_screen() 根据这个枚举值来决定切换到哪个屏幕，
// process_ui_command() 根据这个枚举值来决定把手势分发给谁。
typedef enum {
    SCREEN_MAIN_AR,      //  1. AR 主视界 / 待机表盘（首页）
    SCREEN_MENU,         //  2. 滚动图标式主菜单
    SCREEN_CLOCK,        //  3. 翻页时钟 / 秒表小工具
    SCREEN_RECORD,       //  4. 录音机界面
    SCREEN_PLAYLIST,     //  5. 录音回放列表界面
    SCREEN_CAMERA,       //  6. 目标追踪 / 全息相机界面
    SCREEN_MUSIC,        //  7. 骨传导音乐播放器控制
    SCREEN_PITCH,        //  8. IMU 姿态仪 / 魔法棒校准
    SCREEN_NOISE,        //  9. 白噪音深度睡眠专注模式
    SCREEN_NOVEL,        // 10. AI 小说 / AI 智能对话交互
    SCREEN_GAME_LIST,    // 11. 游戏中心子菜单列表
    SCREEN_GAME,         // 12. 赛博跑酷
    SCREEN_GAME_2048,    // 13. 经典体感 2048
    SCREEN_GAME_FLAPPY,  // 14. 像素鸟
    SCREEN_GAME_NOTE,    // 15. 声控八分音符酱
    SCREEN_GAME_TETRIS,  // 16. 俄罗斯方块
    SCREEN_GAME_MOLE,    // 17. 打地鼠
    SCREEN_GAME_SNAKE,   // 18. 贪吃蛇
    SCREEN_GAME_RHYTHM,  // 19. 节奏魔杖
    SCREEN_GAME_SIMON,   // 20. 记忆大师
    SCREEN_LIGHT,        // 18. 光照传感器界面
    SCREEN_HEALTH        // 19. 心率血氧监测
} ui_screen_state_t;

// ============================================================
//   对外接口函数声明
// ============================================================

// 切换到指定屏幕（状态机核心函数）
// 参数 target_screen: 目标屏幕的枚举值
// 内部会处理：旧屏幕的资源清理（如关闭麦克风）、
//             新屏幕的加载动画、以及屏幕相关的 UART 指令发送。
void switch_to_screen(ui_screen_state_t target_screen);

// UI 管理器初始化（在 main.c 的 app_main 中调用）
// 内部会：初始化所有屏幕模块、创建 FreeRTOS 消息队列、
//         启动 UI 守护任务（负责接收手势并分发）。
void ui_manager_init(void);

// ============================================================
//   手势命令消息队列
// ============================================================
// 这是一个 FreeRTOS 队列，用于在不同任务间传递手势指令。
// BLE 蓝牙任务或 UART 串口任务收到魔杖的手势数据后，
// 通过 xQueueSend() 将 ui_cmd_t 类型的指令投入此队列。
// UI 守护任务（ui_manager_task）通过 xQueueReceive() 取出指令，
// 然后调用 process_ui_command() 分发到当前屏幕。
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
extern QueueHandle_t ui_cmd_queue;

#endif // UI_MANAGER_H
