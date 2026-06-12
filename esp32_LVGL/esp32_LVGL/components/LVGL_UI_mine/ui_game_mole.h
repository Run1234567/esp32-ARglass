#ifndef UI_GAME_MOLE_H
#define UI_GAME_MOLE_H

#include "ui_globals.h"

extern lv_obj_t * ui_game_mole_screen;

void ui_game_mole_init(void);
void game_mole_screen_handle_cmd(ui_cmd_t cmd);
void game_mole_pause_timer(void);

#endif
