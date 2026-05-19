#ifndef _UI_NOVEL_SCREEN_H
#define _UI_NOVEL_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h" // 引入 ui_cmd_t 
#include "ui_manager.h" 
extern lv_obj_t * ui_novel_screen;
extern lv_obj_t * label_novel_text;

// 1. 初始化小说屏幕
void ui_novel_screen_init(void);
void novel_scroll_one_line(void);

// ? 新增：小说模块专属的按键处理接口 
void novel_screen_handle_cmd(ui_cmd_t cmd);

#endif