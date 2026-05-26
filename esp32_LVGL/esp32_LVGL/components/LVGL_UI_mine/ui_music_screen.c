#include "ui_music_screen.h"
#include "my_uart.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

lv_obj_t * ui_music_screen;
static lv_obj_t * obj_list_view;
static lv_obj_t * obj_player_view;
static lv_obj_t * obj_menu_overlay;

static lv_obj_t * roller_playlist;
static lv_obj_t * label_title;
static lv_obj_t * label_lrc_prev;
static lv_obj_t * label_lrc_curr;
static lv_obj_t * label_lrc_next;
static lv_obj_t * slider_progress;
static lv_obj_t * label_time_info;
static lv_obj_t * btn_play_pause;
static lv_obj_t * label_play_icon;
static lv_obj_t * slider_vol;

static char playlist_buf[2048] = "";
static bool is_playing = false;

typedef enum {
    MUSIC_VIEW_LIST,
    MUSIC_VIEW_PLAYER,
    MUSIC_VIEW_MENU
} music_view_t;

static music_view_t current_view = MUSIC_VIEW_LIST;

int music_total_time = 0;
int music_current_time = 0;
char music_current_song[64] = "未选择歌曲";

typedef struct {
    int time_sec;
    char *text;
} lrc_line_t;

static lrc_line_t *lrc_array = NULL;
static int lrc_count = 0;

void music_clear_lrc(void) {
    if (lvgl_port_lock(0)) {
        if (lrc_array != NULL) {
            for (int i = 0; i < lrc_count; i++) {
                if(lrc_array[i].text) free(lrc_array[i].text);
            }
            free(lrc_array);
            lrc_array = NULL;
        }
        lrc_count = 0;

        if(label_lrc_prev) lv_label_set_text(label_lrc_prev, "");
        if(label_lrc_curr) lv_label_set_text(label_lrc_curr, "匹配歌词中...");
        if(label_lrc_next) lv_label_set_text(label_lrc_next, "");

        lvgl_port_unlock();
    }
}

void music_add_lrc_line(int sec, const char* text) {
    if (lvgl_port_lock(0)) {
        lrc_line_t *temp = realloc(lrc_array, (lrc_count + 1) * sizeof(lrc_line_t));
        if (temp != NULL) {
            lrc_array = temp;
            lrc_array[lrc_count].time_sec = sec;
            lrc_array[lrc_count].text = malloc(strlen(text) + 1);
            if (lrc_array[lrc_count].text) {
                strcpy(lrc_array[lrc_count].text, text);
            }
            lrc_count++;
        }
        lvgl_port_unlock();
    }
}

static void update_lrc_display(int sec) {
    if (label_lrc_curr == NULL) return;

    if (lrc_count == 0) {
        lv_label_set_text(label_lrc_prev, "");
        lv_label_set_text(label_lrc_curr, "未找到本地歌词文件");
        lv_label_set_text(label_lrc_next, "");
        return;
    }

    int target_index = 0;
    for (int i = 0; i < lrc_count; i++) {
        if (sec >= lrc_array[i].time_sec) {
            target_index = i;
        } else {
            break;
        }
    }

    static int last_lrc_index = -1;
    if (target_index == last_lrc_index) return;
    last_lrc_index = target_index;

    const char *text_prev = (target_index > 0) ? lrc_array[target_index - 1].text : "";
    const char *text_curr = lrc_array[target_index].text;
    const char *text_next = (target_index < lrc_count - 1) ? lrc_array[target_index + 1].text : "";

    lv_label_set_text(label_lrc_prev, text_prev);
    lv_label_set_text(label_lrc_curr, text_curr);
    lv_label_set_text(label_lrc_next, text_next);
}

static void switch_view(music_view_t view) {
    lv_obj_add_flag(obj_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(obj_player_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(obj_menu_overlay, LV_OBJ_FLAG_HIDDEN);

    switch (view) {
        case MUSIC_VIEW_LIST:
            lv_obj_clear_flag(obj_list_view, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_VIEW_PLAYER:
            lv_obj_clear_flag(obj_player_view, LV_OBJ_FLAG_HIDDEN);
            break;
        case MUSIC_VIEW_MENU:
            lv_obj_clear_flag(obj_player_view, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(obj_menu_overlay, LV_OBJ_FLAG_HIDDEN);
            break;
    }
    current_view = view;
}

static void do_play_selected(void) {
    if (roller_playlist == NULL) return;
    char selected_song[64];
    lv_roller_get_selected_str(roller_playlist, selected_song, sizeof(selected_song));

    strncpy(music_current_song, selected_song, sizeof(music_current_song) - 1);
    music_current_song[sizeof(music_current_song) - 1] = '\0';

    is_playing = true;
    if (label_play_icon != NULL) lv_label_set_text(label_play_icon, LV_SYMBOL_PAUSE);
    if (label_title != NULL) lv_label_set_text(label_title, music_current_song);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "CMD:PLAY_YY:%s\r\n", selected_song);
    my_uart_send(cmd);
}

void music_screen_play_selected(void) {
    do_play_selected();
}

static void toggle_play_pause(void) {
    if (is_playing) {
        my_uart_send("CMD:PAUSE_MUSIC\r\n");
        if (label_play_icon != NULL) lv_label_set_text(label_play_icon, LV_SYMBOL_PLAY);
    } else {
        my_uart_send("CMD:RESUME_MUSIC\r\n");
        if (label_play_icon != NULL) lv_label_set_text(label_play_icon, LV_SYMBOL_PAUSE);
    }
    is_playing = !is_playing;
}

void music_screen_handle_cmd(ui_cmd_t cmd) {
    switch (current_view) {
        case MUSIC_VIEW_LIST:
            if (cmd == UI_CMD_UP) {
                uint16_t idx = lv_roller_get_selected(roller_playlist);
                if (idx > 0) lv_roller_set_selected(roller_playlist, idx - 1, LV_ANIM_ON);
            }
            else if (cmd == UI_CMD_DOWN) {
                uint16_t idx = lv_roller_get_selected(roller_playlist);
                lv_roller_set_selected(roller_playlist, idx + 1, LV_ANIM_ON);
            }
            else if (cmd == UI_CMD_RIGHT) {
                do_play_selected();
                switch_view(MUSIC_VIEW_PLAYER);
            }
            else if (cmd == UI_CMD_LEFT) {
                extern void switch_to_screen(ui_screen_state_t target);
                switch_to_screen(SCREEN_MENU);
            }
            break;

        case MUSIC_VIEW_PLAYER:
            if (cmd == UI_CMD_LEFT) {
                switch_view(MUSIC_VIEW_LIST);
            }
            else if (cmd == UI_CMD_RIGHT) {
                switch_view(MUSIC_VIEW_MENU);
            }
            else if (cmd == UI_CMD_UP) {
                int new_time = music_current_time + 10;
                if (new_time > music_total_time) new_time = music_total_time;
                char seek_cmd[32];
                snprintf(seek_cmd, sizeof(seek_cmd), "CMD:SEEK_MUSIC:%d\r\n", new_time);
                my_uart_send(seek_cmd);
                lv_slider_set_value(slider_progress, new_time, LV_ANIM_ON);
            }
            else if (cmd == UI_CMD_DOWN) {
                int new_time = music_current_time - 10;
                if (new_time < 0) new_time = 0;
                char seek_cmd[32];
                snprintf(seek_cmd, sizeof(seek_cmd), "CMD:SEEK_MUSIC:%d\r\n", new_time);
                my_uart_send(seek_cmd);
                lv_slider_set_value(slider_progress, new_time, LV_ANIM_ON);
            }
            break;

        case MUSIC_VIEW_MENU:
            if (cmd == UI_CMD_LEFT) {
                switch_view(MUSIC_VIEW_PLAYER);
            }
            else if (cmd == UI_CMD_RIGHT) {
                toggle_play_pause();
            }
            else if (cmd == UI_CMD_UP) {
                int v = lv_slider_get_value(slider_vol);
                v = (v + 10 > 100) ? 100 : v + 10;
                lv_slider_set_value(slider_vol, v, LV_ANIM_ON);
                char vol_cmd[32];
                snprintf(vol_cmd, sizeof(vol_cmd), "CMD:VOL:%d\r\n", v);
                my_uart_send(vol_cmd);
            }
            else if (cmd == UI_CMD_DOWN) {
                int v = lv_slider_get_value(slider_vol);
                v = (v - 10 < 0) ? 0 : v - 10;
                lv_slider_set_value(slider_vol, v, LV_ANIM_ON);
                char vol_cmd[32];
                snprintf(vol_cmd, sizeof(vol_cmd), "CMD:VOL:%d\r\n", v);
                my_uart_send(vol_cmd);
            }
            break;
    }
}

void ui_music_screen_init(void) {
    ui_music_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_music_screen, lv_color_hex(0x000000), 0);

    obj_list_view = lv_obj_create(ui_music_screen);
    lv_obj_set_size(obj_list_view, 240, 240);
    lv_obj_set_style_bg_color(obj_list_view, lv_color_hex(0x101015), 0);
    lv_obj_set_style_border_width(obj_list_view, 0, 0);
    lv_obj_set_style_pad_all(obj_list_view, 0, 0);

    lv_obj_t * list_title = lv_label_create(obj_list_view);
    lv_label_set_text(list_title, "本地音乐舱");
    lv_obj_set_style_text_font(list_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(list_title, LV_ALIGN_TOP_MID, 0, 5);

    roller_playlist = lv_roller_create(obj_list_view);
    lv_obj_set_style_text_font(roller_playlist, &my_font_cn_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller_playlist, &my_font_cn_16, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller_playlist, lv_color_hex(0x101015), 0);
    lv_obj_set_style_border_width(roller_playlist, 0, 0);
    lv_obj_set_style_text_color(roller_playlist, lv_color_hex(0x8888AA), 0);
    lv_obj_set_style_text_color(roller_playlist, lv_color_hex(0x00FFCC), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller_playlist, lv_color_hex(0x222244), LV_PART_SELECTED);
    lv_roller_set_options(roller_playlist, "正在加载歌单...", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller_playlist, 4);
    lv_obj_set_width(roller_playlist, 220);
    lv_obj_align(roller_playlist, LV_ALIGN_CENTER, 0, 10);

    obj_player_view = lv_obj_create(ui_music_screen);
    lv_obj_set_size(obj_player_view, 240, 240);
    lv_obj_set_style_bg_color(obj_player_view, lv_color_hex(0x050508), 0);
    lv_obj_set_style_border_width(obj_player_view, 0, 0);
    lv_obj_set_style_pad_all(obj_player_view, 0, 0);
    lv_obj_add_flag(obj_player_view, LV_OBJ_FLAG_HIDDEN);

    label_title = lv_label_create(obj_player_view);
    lv_label_set_text(label_title, music_current_song);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 15);
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label_title, 220);

    label_lrc_prev = lv_label_create(obj_player_view);
    lv_obj_set_width(label_lrc_prev, 220);
    lv_label_set_long_mode(label_lrc_prev, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label_lrc_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_lrc_prev, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_lrc_prev, lv_color_hex(0x666666), 0);
    lv_obj_align(label_lrc_prev, LV_ALIGN_CENTER, 0, -35);

    label_lrc_curr = lv_label_create(obj_player_view);
    lv_obj_set_width(label_lrc_curr, 220);
    lv_label_set_long_mode(label_lrc_curr, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(label_lrc_curr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_lrc_curr, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_lrc_curr, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(label_lrc_curr, LV_ALIGN_CENTER, 0, -10);

    label_lrc_next = lv_label_create(obj_player_view);
    lv_obj_set_width(label_lrc_next, 220);
    lv_label_set_long_mode(label_lrc_next, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label_lrc_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_lrc_next, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_lrc_next, lv_color_hex(0x666666), 0);
    lv_obj_align(label_lrc_next, LV_ALIGN_CENTER, 0, 15);

    lv_label_set_text(label_lrc_prev, "");
    lv_label_set_text(label_lrc_curr, "准备中...");
    lv_label_set_text(label_lrc_next, "");

    slider_progress = lv_slider_create(obj_player_view);
    lv_obj_set_width(slider_progress, 200);
    lv_slider_set_range(slider_progress, 0, 100);
    lv_obj_align(slider_progress, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x333344), 0);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x00FFCC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x00FFCC), LV_PART_KNOB);
    lv_obj_clear_flag(slider_progress, LV_OBJ_FLAG_CLICKABLE);

    label_time_info = lv_label_create(obj_player_view);
    lv_label_set_text(label_time_info, "00:00 / 00:00");
    lv_obj_set_style_text_font(label_time_info, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_time_info, lv_color_hex(0x888888), 0);
    lv_obj_align(label_time_info, LV_ALIGN_BOTTOM_MID, 0, -45);

    obj_menu_overlay = lv_obj_create(ui_music_screen);
    lv_obj_set_size(obj_menu_overlay, 120, 240);
    lv_obj_align(obj_menu_overlay, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(obj_menu_overlay, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(obj_menu_overlay, 230, 0);
    lv_obj_set_style_border_width(obj_menu_overlay, 0, 0);
    lv_obj_set_style_pad_all(obj_menu_overlay, 5, 0);
    lv_obj_add_flag(obj_menu_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * menu_label = lv_label_create(obj_menu_overlay);
    lv_label_set_text(menu_label, "控制");
    lv_obj_set_style_text_font(menu_label, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0x00FFCC), 0);
    lv_obj_align(menu_label, LV_ALIGN_TOP_MID, 0, 10);

    btn_play_pause = lv_btn_create(obj_menu_overlay);
    lv_obj_set_size(btn_play_pause, 50, 50);
    lv_obj_align(btn_play_pause, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(btn_play_pause, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_play_pause, lv_color_hex(0xFF973B), 0);

    label_play_icon = lv_label_create(btn_play_pause);
    lv_label_set_text(label_play_icon, LV_SYMBOL_PAUSE);
    lv_obj_center(label_play_icon);

    lv_obj_t * vol_lbl = lv_label_create(obj_menu_overlay);
    lv_label_set_text(vol_lbl, "音量");
    lv_obj_set_style_text_font(vol_lbl, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(vol_lbl, lv_color_hex(0x8888AA), 0);
    lv_obj_align(vol_lbl, LV_ALIGN_TOP_MID, 0, 110);

    slider_vol = lv_slider_create(obj_menu_overlay);
    lv_slider_set_range(slider_vol, 0, 100);
    lv_slider_set_value(slider_vol, 80, LV_ANIM_OFF);
    lv_obj_set_width(slider_vol, 100);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_MID, 0, 135);
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(0x333344), 0);
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(0xFF973B), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(0xFF973B), LV_PART_KNOB);
    lv_obj_clear_flag(slider_vol, LV_OBJ_FLAG_CLICKABLE);

    current_view = MUSIC_VIEW_LIST;
}

void music_clear_playlist(void) {
    if (lvgl_port_lock(0)) {
        playlist_buf[0] = '\0';
        lvgl_port_unlock();
    }
}

void music_add_song(const char* song_name) {
    if (lvgl_port_lock(0)) {
        if (strlen(playlist_buf) + strlen(song_name) + 2 < sizeof(playlist_buf)) {
            strcat(playlist_buf, song_name);
            strcat(playlist_buf, "\n");
        }
        lvgl_port_unlock();
    }
}

void music_apply_playlist(void) {
    if (lvgl_port_lock(0)) {
        size_t len = strlen(playlist_buf);
        if (len > 0 && playlist_buf[len - 1] == '\n') {
            playlist_buf[len - 1] = '\0';
        }

        if (strlen(playlist_buf) > 0) {
            lv_roller_set_options(roller_playlist, playlist_buf, LV_ROLLER_MODE_NORMAL);
        } else {
            lv_roller_set_options(roller_playlist, "文件夹为空", LV_ROLLER_MODE_NORMAL);
        }
        lvgl_port_unlock();
    }
}

void music_set_total_time(int t_sec) {
    if (lvgl_port_lock(0)) {
        music_total_time = t_sec;
        if (slider_progress != NULL) {
            lv_slider_set_range(slider_progress, 0, t_sec);
        }
        lvgl_port_unlock();
    }
}

void music_update_progress(int cur_sec) {
    if (lvgl_port_lock(0)) {
        music_current_time = cur_sec;
        if (slider_progress != NULL) {
            lv_slider_set_value(slider_progress, cur_sec, LV_ANIM_ON);
        }
        if (label_time_info != NULL) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d",
                     cur_sec / 60, cur_sec % 60,
                     music_total_time / 60, music_total_time % 60);
            lv_label_set_text(label_time_info, buf);
        }
        update_lrc_display(cur_sec);
        lvgl_port_unlock();
    }
}
