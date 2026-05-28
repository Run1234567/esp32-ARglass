#ifndef _UI_NOVEL_SCREEN_H
#define _UI_NOVEL_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h" // ???? ui_cmd_t 
#include "ui_manager.h" 
extern lv_obj_t * ui_novel_screen;
extern lv_obj_t * label_novel_text;

// 1. ?????��????
void ui_novel_screen_init(void);
void novel_scroll_one_line(void);
void novel_screen_handle_cmd(ui_cmd_t cmd);

void novel_ui_clear_book_list(void);
void novel_ui_add_book(const char* name);
void novel_ui_clear_chap_list(void);
void novel_ui_add_chap(const char* name);
void novel_ui_add_book_page_btn(int is_next);
void novel_ui_add_chap_page_btn(int is_next);

#endif