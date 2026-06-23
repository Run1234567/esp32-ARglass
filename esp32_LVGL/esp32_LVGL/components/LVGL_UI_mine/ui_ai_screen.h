#ifndef UI_AI_SCREEN_H
#define UI_AI_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_ai_screen;

void ui_ai_screen_init(void);
void ui_update_ai_text(const char *text);
void ai_screen_handle_cmd(ui_cmd_t cmd);

#endif
