#ifndef _UI_VIDEO_SCREEN_H
#define _UI_VIDEO_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_video_screen;

void ui_video_screen_init(void);
void video_screen_handle_cmd(ui_cmd_t cmd);

#endif
