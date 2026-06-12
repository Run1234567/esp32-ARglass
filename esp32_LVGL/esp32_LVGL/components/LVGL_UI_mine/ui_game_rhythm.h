#ifndef UI_GAME_RHYTHM_H
#define UI_GAME_RHYTHM_H

#include "ui_globals.h"

extern lv_obj_t * ui_game_rhythm_screen;

void ui_game_rhythm_init(void);
void game_rhythm_screen_handle_cmd(ui_cmd_t cmd);
void game_rhythm_pause_timer(void);

#endif
