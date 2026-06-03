#include "ui_globals.h"
#include "esp_lvgl_port.h"
#include "ui_manager.h"

#define GAME_ITEM_COUNT 2

lv_obj_t * ui_game_list_screen;
static lv_obj_t * game_roller;

void ui_game_list_screen_init(void) {
    ui_game_list_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_list_screen, lv_color_black(), 0);
    
    // 顶部标题
    lv_obj_t * label_title = lv_label_create(ui_game_list_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(label_title, "游戏中心");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    // 游戏列表滚轮
    game_roller = lv_roller_create(ui_game_list_screen);
    lv_roller_set_options(game_roller,
                        "🏃 赛博跑酷\n"
                        "👾 敬请期待...",
                        LV_ROLLER_MODE_NORMAL);

    lv_roller_set_visible_row_count(game_roller, 2);
    lv_obj_center(game_roller);
    lv_obj_set_width(game_roller, 200);
    lv_obj_set_style_text_font(game_roller, &my_font_cn_16, 0);
    
    // 样式调整
    lv_obj_set_style_bg_color(game_roller, lv_color_black(), 0);
    lv_obj_set_style_border_width(game_roller, 0, 0);
    lv_obj_set_style_text_color(game_roller, lv_color_hex(0x888888), 0);
    lv_obj_set_style_bg_color(game_roller, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_text_color(game_roller, lv_color_black(), LV_PART_SELECTED);
}

// 处理游戏列表界面的手势
void game_list_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_UP) {
        uint16_t current_idx = lv_roller_get_selected(game_roller);
        if (current_idx > 0) lv_roller_set_selected(game_roller, current_idx - 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_DOWN) {
        uint16_t current_idx = lv_roller_get_selected(game_roller);
        if (current_idx < GAME_ITEM_COUNT - 1) lv_roller_set_selected(game_roller, current_idx + 1, LV_ANIM_ON);
    }
    else if (cmd == UI_CMD_LEFT) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
    else if (cmd == UI_CMD_RIGHT) {
        uint16_t selected_idx = lv_roller_get_selected(game_roller);
        if (selected_idx == 0) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME);
        }
    }
}
