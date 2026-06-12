#ifndef UI_GAME_SNAKE_H
#define UI_GAME_SNAKE_H

#include "ui_globals.h"

extern lv_obj_t * ui_game_snake_screen;

void ui_game_snake_init(void);
void game_snake_screen_handle_cmd(ui_cmd_t cmd);
void game_snake_pause_timer(void);

#endif
