#include "ui_camera_screen.h"
#include "my_uart.h"
#include "esp_lvgl_port.h"
#include "ui_manager.h"
#include "esp_log.h"

static const char *TAG = "CAM";

lv_obj_t * ui_camera_screen;
static lv_obj_t * label_cam_status;

void camera_screen_handle_cmd(uint8_t cmd) {
    if (cmd == UI_CMD_RIGHT) {
        my_uart_send("CMD:TAKE_PHOTO\r\n");
        lv_label_set_text(label_cam_status, "#FFFF00 正在保存至 SD 卡...#");
    }
    else if (cmd == UI_CMD_LEFT) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}

void ui_camera_screen_init(void) {
    ui_camera_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_camera_screen, lv_color_black(), 0);

    lv_obj_t *label_title = lv_label_create(ui_camera_screen);
    lv_label_set_recolor(label_title, true);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_label_set_text(label_title, "#FFFFFF 📷 相机模式#");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *label_hint = lv_label_create(ui_camera_screen);
    lv_label_set_recolor(label_hint, true);
    lv_obj_set_style_text_font(label_hint, &my_font_cn_16, 0);
    lv_label_set_text(label_hint, "#AAAAAA 右滑拍照，左滑返回#");
    lv_obj_align(label_hint, LV_ALIGN_CENTER, 0, 0);

    label_cam_status = lv_label_create(ui_camera_screen);
    lv_label_set_recolor(label_cam_status, true);
    lv_obj_set_style_text_font(label_cam_status, &my_font_cn_16, 0);
    lv_label_set_text(label_cam_status, "#00FF00 就绪#");
    lv_obj_align(label_cam_status, LV_ALIGN_BOTTOM_MID, 0, -5);
}

void camera_reset_status_label(void) {
    if (lvgl_port_lock(0)) {
        lv_label_set_text(label_cam_status, "#00FF00 就绪#");
        lvgl_port_unlock();
    }
}
