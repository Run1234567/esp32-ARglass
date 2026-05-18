#ifndef _UI_CLOCK_SCREEN_H
#define _UI_CLOCK_SCREEN_H

#include "lvgl.h"
#include "ui_manager.h" // 引入全局指令枚举

// 初始化时钟屏幕句柄和基础UI
void ui_clock_screen_init(void);

// 统一接收并处理魔杖指令的接口
void clock_screen_handle_cmd(ui_cmd_t cmd);

#endif // _UI_CLOCK_SCREEN_H
