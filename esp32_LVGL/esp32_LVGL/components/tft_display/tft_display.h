#ifndef _TFT_DISPLAY_H_
#define _TFT_DISPLAY_H_

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// ±©Â¶¾ä±ú¸ø LVGL Ê¹ÓÃ
extern esp_lcd_panel_io_handle_t io_handle;
extern esp_lcd_panel_handle_t panel_handle;

void lcd_init(void);

#endif