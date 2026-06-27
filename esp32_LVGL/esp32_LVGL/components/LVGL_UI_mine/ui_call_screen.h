#ifndef UI_CALL_SCREEN_H
#define UI_CALL_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_call_screen;
extern volatile bool is_calling_now;

void ui_call_screen_init(void);
void call_screen_handle_cmd(ui_cmd_t cmd);

#endif
