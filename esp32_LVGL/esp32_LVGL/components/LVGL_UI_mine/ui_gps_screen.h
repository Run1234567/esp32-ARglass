#ifndef UI_GPS_SCREEN_H
#define UI_GPS_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_gps_screen;

void ui_gps_screen_init(void);
void gps_screen_handle_cmd(ui_cmd_t cmd);
void ui_gps_start_update(void);
void ui_gps_stop_update(void);

#endif
