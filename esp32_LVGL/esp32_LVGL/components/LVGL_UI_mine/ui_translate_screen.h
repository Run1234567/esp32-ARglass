#ifndef _UI_TRANSLATE_SCREEN_H
#define _UI_TRANSLATE_SCREEN_H

#include "lvgl.h"

void ui_translate_screen_init(void);
void ui_update_translate_text(const char *text);
void ui_enter_translate_mode(void);
void ui_exit_translate_mode(void);
void translate_screen_handle_cmd(int cmd);

#endif
