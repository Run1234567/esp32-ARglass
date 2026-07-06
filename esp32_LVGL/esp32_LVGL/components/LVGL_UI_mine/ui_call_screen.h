#ifndef UI_CALL_SCREEN_H
#define UI_CALL_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_call_screen;
extern lv_obj_t * ui_ring_screen;
extern volatile bool is_calling_now;

void ui_call_screen_init(void);
void call_screen_handle_cmd(ui_cmd_t cmd);

// 供串口任务调用的通话状态控制 API
void ui_enter_ringing_mode(void);
void ui_call_established(void);
void ui_call_ended(void);

#endif
