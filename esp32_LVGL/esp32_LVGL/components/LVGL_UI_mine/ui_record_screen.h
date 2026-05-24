#ifndef _UI_RECORD_SCREEN_H
#define _UI_RECORD_SCREEN_H

#include "lvgl.h"
#include "ui_globals.h"

extern lv_obj_t * ui_record_screen;

void ui_record_screen_init(void);

// ¼������ר��������·��
void record_screen_handle_cmd(uint8_t cmd);

#endif // _UI_RECORD_SCREEN_H
