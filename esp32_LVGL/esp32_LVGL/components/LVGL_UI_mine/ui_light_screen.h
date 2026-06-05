#ifndef UI_LIGHT_SCREEN_H
#define UI_LIGHT_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_light_screen;

void ui_light_screen_init(void);
void light_screen_handle_cmd(ui_cmd_t cmd);

#endif
