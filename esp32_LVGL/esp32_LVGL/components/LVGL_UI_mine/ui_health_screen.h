#ifndef UI_HEALTH_SCREEN_H
#define UI_HEALTH_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_health_screen;

void ui_health_screen_init(void);
void health_screen_handle_cmd(ui_cmd_t cmd);

#endif
