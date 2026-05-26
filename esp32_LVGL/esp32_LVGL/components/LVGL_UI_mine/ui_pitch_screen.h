#ifndef _UI_PITCH_SCREEN_H
#define _UI_PITCH_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"

extern lv_obj_t * ui_pitch_screen;

void ui_pitch_screen_init(void);
void update_pitch_ui(float freq);

#endif
