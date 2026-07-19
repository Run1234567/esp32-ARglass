#ifndef UI_TRANSLATE_LANG_SCREEN_H
#define UI_TRANSLATE_LANG_SCREEN_H

#include "lvgl.h"

extern lv_obj_t * ui_translate_lang_screen;

void ui_translate_lang_screen_init(void);
void translate_lang_screen_handle_cmd(int cmd);

#endif // UI_TRANSLATE_LANG_SCREEN_H
