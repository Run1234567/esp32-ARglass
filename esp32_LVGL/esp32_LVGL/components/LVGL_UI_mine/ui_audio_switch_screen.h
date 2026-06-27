#ifndef UI_AUDIO_SWITCH_SCREEN_H
#define UI_AUDIO_SWITCH_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_audio_switch_screen;

void ui_audio_switch_screen_init(void);
void audio_switch_handle_cmd(ui_cmd_t cmd);

#endif
