#ifndef UI_TRANSLATE_MODE_SCREEN_H
#define UI_TRANSLATE_MODE_SCREEN_H

#include "lvgl.h"

extern lv_obj_t * ui_translate_mode_screen;

void ui_translate_mode_screen_init(void);
void translate_mode_screen_handle_cmd(int cmd);

#endif // UI_TRANSLATE_MODE_SCREEN_H
