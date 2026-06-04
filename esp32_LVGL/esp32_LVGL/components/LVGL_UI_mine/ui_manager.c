#include "ui_manager.h"
#include "ui_globals.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

// Include all screen modules
#include "ui_ar_glass.h"
#include "ui_menu_screen.h"
#include "ui_novel_screen.h"
#include "ui_clock_screen.h"
#include "ui_record_screen.h"
#include "ui_playlist_screen.h"
#include "ui_camera_screen.h"
#include "ui_noise_screen.h"
#include "ui_pitch_screen.h"
#include "ui_music_screen.h"
#include "my_uart.h"

extern void ui_game_screen_init(void);
extern void ui_game_2048_init(void);
extern void game_2048_screen_handle_cmd(ui_cmd_t cmd);
extern void game_screen_handle_cmd(ui_cmd_t cmd);
extern void ui_game_list_screen_init(void);
extern void game_list_screen_handle_cmd(ui_cmd_t cmd);

static const char *TAG = "UI_MANAGER";

QueueHandle_t ui_cmd_queue = NULL;
static ui_screen_state_t current_screen = SCREEN_MAIN_AR;

// Screen switch engine
void switch_to_screen(ui_screen_state_t target_screen) {
    if (current_screen == target_screen) return;

    lv_obj_t * target_obj = NULL;

    switch (target_screen) {
        case SCREEN_MAIN_AR: target_obj = ui_main_screen; break;
        case SCREEN_MENU:    target_obj = ui_menu_screen; break;
        case SCREEN_NOVEL:   target_obj = ui_novel_screen; break;
        case SCREEN_CLOCK:   target_obj = ui_clock_screen; break;
        case SCREEN_RECORD:  target_obj = ui_record_screen; break;
        case SCREEN_PLAYLIST: target_obj = ui_playlist_screen; break;
        case SCREEN_CAMERA:  target_obj = ui_camera_screen; break;
        case SCREEN_NOISE:   target_obj = ui_noise_screen; break;
        case SCREEN_PITCH:   target_obj = ui_pitch_screen; break;
        case SCREEN_MUSIC:   target_obj = ui_music_screen; break;
        case SCREEN_GAME_LIST: target_obj = ui_game_list_screen; break;
        case SCREEN_GAME:    target_obj = ui_game_screen; break;
        case SCREEN_GAME_2048: target_obj = ui_game_2048_screen; break;
        default: return;
    }

    if (target_screen == SCREEN_PLAYLIST) {
        my_uart_send("CMD:GET_REC_LIST\r\n");
    }

    if (target_screen == SCREEN_MUSIC) {
        my_uart_send("CMD:GET_MUSIC_LIST\r\n");
    }

    if (target_screen == SCREEN_NOVEL) {
        my_uart_send("CMD:GET_BOOKS\r\n");
    }

    if (current_screen == SCREEN_NOISE && target_screen != SCREEN_NOISE) {
        my_uart_send("CMD:NOISE_OFF\r\n");
    }

    if (current_screen == SCREEN_PITCH && target_screen != SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_OFF\r\n");
    }

    lv_scr_load_anim(target_obj, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    current_screen = target_screen;

    if (current_screen == SCREEN_NOISE) {
        my_uart_send("CMD:NOISE_ON\r\n");
    }

    if (current_screen == SCREEN_PITCH) {
        my_uart_send("CMD:PITCH_ON\r\n");
    }

    ESP_LOGI(TAG, "Screen switched to: %d", current_screen);
}

// Command dispatcher
static void process_ui_command(ui_cmd_t cmd) {
    switch (current_screen) {
        
        case SCREEN_MAIN_AR:
            if (cmd == UI_CMD_RIGHT) {
                switch_to_screen(SCREEN_MENU);
            }
            break;

        case SCREEN_MENU:
            if (cmd == UI_CMD_UP) {
                menu_scroll_up();
            }
            else if (cmd == UI_CMD_DOWN) {
                menu_scroll_down();
            }
            else if (cmd == UI_CMD_LEFT) {
                switch_to_screen(SCREEN_MAIN_AR);
            }
            else if (cmd == UI_CMD_RIGHT) {
                uint16_t selected_idx = lv_roller_get_selected(menu_roller);
                if (selected_idx == 0) switch_to_screen(SCREEN_MAIN_AR);
                if (selected_idx == 2) switch_to_screen(SCREEN_CLOCK);
                if (selected_idx == 3) switch_to_screen(SCREEN_RECORD);
                if (selected_idx == 4) switch_to_screen(SCREEN_PLAYLIST);
                if (selected_idx == 5) switch_to_screen(SCREEN_CAMERA);
                if (selected_idx == 7) switch_to_screen(SCREEN_MUSIC);
                if (selected_idx == 8) switch_to_screen(SCREEN_PITCH);
                if (selected_idx == 9) switch_to_screen(SCREEN_NOISE);
                if (selected_idx == 10) switch_to_screen(SCREEN_NOVEL);
                if (selected_idx == 11) switch_to_screen(SCREEN_GAME_LIST);
            }
            break;

        case SCREEN_NOVEL:
            novel_screen_handle_cmd(cmd);
            break;

        case SCREEN_CLOCK:
            clock_screen_handle_cmd(cmd);
            break;

        case SCREEN_RECORD:
            record_screen_handle_cmd(cmd);
            break;

        case SCREEN_PLAYLIST:
            playlist_screen_handle_cmd(cmd);
            break;

        case SCREEN_CAMERA:
            camera_screen_handle_cmd(cmd);
            break;

        case SCREEN_NOISE:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            break;

        case SCREEN_PITCH:
            if (cmd == UI_CMD_LEFT) switch_to_screen(SCREEN_MENU);
            break;

        case SCREEN_MUSIC:
            music_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_LIST:
            game_list_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME:
            game_screen_handle_cmd(cmd);
            break;

        case SCREEN_GAME_2048:
            game_2048_screen_handle_cmd(cmd);
            break;

        default:
            break;
    }
}

// UI guardian task
static void ui_manager_task(void *pvParameter) {
    ui_cmd_t received_cmd;

    while (1) {
        if (xQueueReceive(ui_cmd_queue, &received_cmd, portMAX_DELAY) == pdTRUE) {
            if (lvgl_port_lock(0)) {
                process_ui_command(received_cmd);
                lvgl_port_unlock();
            }
        }
    }
}

// Init all UI
void ui_manager_init(void) {
    if (lvgl_port_lock(0)) {
        ui_ar_glass_init();
        ui_menu_screen_init();
        ui_novel_screen_init();
        ui_clock_screen_init();
        ui_record_screen_init();
        ui_playlist_screen_init();
        ui_camera_screen_init();
        ui_noise_screen_init();
        ui_pitch_screen_init();
        ui_music_screen_init();
        ui_game_list_screen_init();
        ui_game_screen_init();
        ui_game_2048_init();
        
        lv_scr_load(ui_main_screen);
        lvgl_port_unlock();
    }

    ui_cmd_queue = xQueueCreate(10, sizeof(ui_cmd_t));
    xTaskCreatePinnedToCore(ui_manager_task, "ui_mgr", 1024 * 4, NULL, 5, NULL, 1);
    
    ESP_LOGI(TAG, "UI Manager initialized!");
}