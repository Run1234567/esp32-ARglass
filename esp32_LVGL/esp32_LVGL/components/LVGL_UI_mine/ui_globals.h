// ui_globals.h
#ifndef _UI_GLOBALS_H
#define _UI_GLOBALS_H

#include "lvgl.h"

// 1. 字体声明
LV_FONT_DECLARE(my_font_cn_16);

// 2. 屏幕对象声明 (方便跨页面切换)
extern lv_obj_t * ui_main_screen;
extern lv_obj_t * ui_menu_screen;

// 3. 需要被 MQTT 动态更新的 UI 元素声明
extern lv_obj_t * label_time;
extern lv_obj_t * label_date;
extern lv_obj_t * label_lunar;
extern lv_obj_t * label_weather;
extern lv_obj_t * label_batt_pct;
extern lv_obj_t * icon_batt;
extern lv_obj_t * menu_roller; // 菜单滚轮
// 小说
extern lv_obj_t * ui_novel_screen;
extern lv_obj_t * label_novel_text;
extern lv_obj_t * novel_scroll_cont;
extern const char * test_novel_text;
extern uint8_t novel_scroll_task_running; // 滚动任务状态标志
extern char * novel_source_buffer; // 书库缓冲区
extern size_t current_book_pos; // 当前书籍位置指针

#endif