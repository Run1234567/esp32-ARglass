#ifndef _UI_PLAYLIST_SCREEN_H
#define _UI_PLAYLIST_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"

extern lv_obj_t * ui_playlist_screen;

void ui_playlist_screen_init(void);
void playlist_screen_handle_cmd(uint8_t cmd);

// 供串口调用的列表更新接口
void playlist_clear(void);
void playlist_add_file(const char* filename);
void playlist_update_ui(void);
void playlist_set_total_time(int t_sec);

#endif
