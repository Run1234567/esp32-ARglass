#ifndef UI_GAME_TETRIS_H
#define UI_GAME_TETRIS_H

#include "ui_globals.h"

extern lv_obj_t * ui_game_tetris_screen;

void ui_game_tetris_init(void);
void game_tetris_screen_handle_cmd(ui_cmd_t cmd);
void game_tetris_pause_timer(void);

#endif
