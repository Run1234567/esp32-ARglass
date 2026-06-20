#ifndef UI_WIFI_SCAN_SCREEN_H
#define UI_WIFI_SCAN_SCREEN_H

#include "ui_globals.h"

extern lv_obj_t * ui_wifi_scan_screen;

void ui_wifi_scan_screen_init(void);
void wifi_scan_screen_handle_cmd(ui_cmd_t cmd);
void ui_wifi_scan_start(void);

#endif
