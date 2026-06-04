#ifndef _UI_CAMERA_SCREEN_H
#define _UI_CAMERA_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"

extern lv_obj_t * ui_camera_screen;

void ui_camera_screen_init(void);
void camera_screen_handle_cmd(ui_cmd_t cmd);

#endif
