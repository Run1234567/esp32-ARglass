#ifndef _UI_NOISE_SCREEN_H
#define _UI_NOISE_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"

extern lv_obj_t * ui_noise_screen;

void ui_noise_screen_init(void);
void update_noise_meter(int val);

#endif
