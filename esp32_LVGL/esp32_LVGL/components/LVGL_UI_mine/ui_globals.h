// ui_globals.h
#ifndef _UI_GLOBALS_H
#define _UI_GLOBALS_H

#include "lvgl.h"

// 1. ????????
LV_FONT_DECLARE(my_font_cn_16);

// 2. ??????????? (????????????)
extern lv_obj_t * ui_main_screen;
extern lv_obj_t * ui_menu_screen;
extern lv_obj_t * ui_clock_screen; // ? ������ʱ�ӹ�����Ļȫ�־��
extern lv_obj_t * ui_playlist_screen; // ? �����������б���Ļȫ�־��

// 3. ????? MQTT ???????? UI ???????
extern lv_obj_t * label_time;
extern lv_obj_t * label_date;
extern lv_obj_t * label_lunar;
extern lv_obj_t * label_weather;
extern lv_obj_t * label_batt_pct;
extern lv_obj_t * icon_batt;
extern lv_obj_t * menu_roller; // ???????
// ???
extern lv_obj_t * ui_novel_screen;
extern lv_obj_t * label_novel_text;
extern lv_obj_t * novel_scroll_cont;
extern const char * test_novel_text;
extern uint8_t novel_scroll_task_running; // ?????????????
extern char * novel_source_buffer; // ???????
extern size_t current_book_pos; // ?????????????

extern int music_total_time;
extern int music_current_time;
extern char music_current_song[64];

#endif