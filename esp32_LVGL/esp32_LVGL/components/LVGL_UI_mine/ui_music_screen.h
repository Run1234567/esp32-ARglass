#ifndef _UI_MUSIC_SCREEN_H
#define _UI_MUSIC_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"
#include "ui_manager.h"

extern lv_obj_t * ui_music_screen;

void ui_music_screen_init(void);
void music_screen_handle_cmd(ui_cmd_t cmd);
void music_screen_play_selected(void);
void music_clear_playlist(void);
void music_add_song(const char* song_name);
void music_apply_playlist(void);
void music_set_total_time(int t_sec);
void music_update_progress(int cur_sec);

#endif
