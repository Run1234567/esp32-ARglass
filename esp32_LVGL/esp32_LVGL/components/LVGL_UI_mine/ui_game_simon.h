#ifndef UI_GAME_SIMON_H
#define UI_GAME_SIMON_H

#include "ui_globals.h"

extern lv_obj_t * ui_game_simon_screen;

void ui_game_simon_init(void);
void game_simon_screen_handle_cmd(ui_cmd_t cmd);
void game_simon_pause_timer(void);

#endif
