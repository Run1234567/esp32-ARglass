/**
 * @file ui_game_simon.c
 * @brief 记忆大师 (Simon Says) —— 脑力与体感结合的魔法记忆训练
 *
 * 视觉风格：全黑背景，十字排列的四个方向区块。
 * 交互逻辑：
 * 【游玩中】：系统先演示序列，玩家凭记忆挥出对应方向。
 * 【控制键】：画圆 = 全局暂停/菜单键。
 * 【暂停中】：上/下切换选项，右挥/画圆确认。
 * 【未开始】：右挥开始，左挥退出。
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdlib.h>

#define MAX_LEVEL 100

// ==========================================
//   全局对象与状态
// ==========================================
lv_obj_t * ui_game_simon_screen;
static lv_obj_t * label_score;
static lv_obj_t * label_msg;
static lv_obj_t * arrows[4]; // 0:上, 1:下, 2:左, 3:右

static lv_obj_t * pause_overlay;
static lv_obj_t * label_pause_options[3];
static bool is_paused = false;
static int pause_choice = 0;

typedef enum {
    STATE_INIT,
    STATE_DEMO,
    STATE_PLAYING,
    STATE_FLASH
} SimonState;

static SimonState game_state = STATE_INIT;
static int sequence[MAX_LEVEL];
static int current_level = 1;
static int demo_idx = 0;
static int player_idx = 0;
static int current_lit_arrow = -1;

static lv_timer_t * game_timer = NULL;
static int tick_counter = 0;

// ==========================================
//   视觉辅助
// ==========================================
static void draw_arrows(void) {
    lv_color_t colors[4] = {
        lv_color_hex(0xFF4444), // 上：红
        lv_color_hex(0x4444FF), // 下：蓝
        lv_color_hex(0x44FF44), // 左：绿
        lv_color_hex(0xFFFF44)  // 右：黄
    };

    for (int i = 0; i < 4; i++) {
        if (i == current_lit_arrow) {
            lv_obj_set_style_bg_color(arrows[i], colors[i], 0);
            lv_obj_set_style_border_color(arrows[i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_color(arrows[i], lv_color_black(), 0);
            lv_obj_set_style_border_color(arrows[i], lv_color_hex(0x555555), 0);
        }
    }
}

static void update_pause_menu(void) {
    const char* texts[3] = {"继续游戏", "重新开始", "退出游戏"};
    for (int i = 0; i < 3; i++) {
        if (i == pause_choice) {
            lv_label_set_text_fmt(label_pause_options[i], "> %s <", texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0x00FFFF), 0);
        } else {
            lv_label_set_text(label_pause_options[i], texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0x808080), 0);
        }
    }
}

static void start_next_level(void) {
    sequence[current_level - 1] = rand() % 4;
    game_state = STATE_DEMO;
    demo_idx = 0;
    tick_counter = 0;
    current_lit_arrow = -1;
    draw_arrows();

    lv_label_set_text_fmt(label_score, "Level: %d", current_level);

    // 系统演示时：青色提示
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(label_msg, "仔细看！系统演示中...");

    int speed = 400 - (current_level * 15);
    if (speed < 150) speed = 150;
    lv_timer_set_period(game_timer, speed);
    lv_timer_resume(game_timer);
}

// ==========================================
//   状态机心跳
// ==========================================
static void simon_tick_cb(lv_timer_t * timer) {
    if (is_paused) return;

    if (lvgl_port_lock(0)) {
        if (game_state == STATE_DEMO) {
            tick_counter++;
            if (tick_counter % 2 == 1) {
                if (demo_idx < current_level) {
                    current_lit_arrow = sequence[demo_idx];
                    draw_arrows();
                }
            } else {
                current_lit_arrow = -1;
                draw_arrows();
                demo_idx++;

                if (demo_idx >= current_level) {
                    game_state = STATE_PLAYING;
                    player_idx = 0;

                    // 玩家回合：绿色提示
                    lv_obj_set_style_text_color(label_msg, lv_color_hex(0x00FF00), 0);
                    lv_label_set_text_fmt(label_msg, "你的回合！请复现动作\n(0/%d)", current_level);

                    lv_timer_pause(game_timer);
                }
            }
        }
        else if (game_state == STATE_FLASH) {
            current_lit_arrow = -1;
            draw_arrows();
            lv_timer_pause(game_timer);

            if (player_idx >= current_level) {
                current_level++;
                start_next_level();
            } else {
                game_state = STATE_PLAYING;
            }
        }
        lvgl_port_unlock();
    }
}

void game_simon_pause_timer(void) {
    is_paused = false;
    if (game_timer) lv_timer_pause(game_timer);
}

// ==========================================
//   初始化
// ==========================================
void ui_game_simon_init(void) {
    ui_game_simon_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_simon_screen, lv_color_black(), 0);

    // 顶部状态板
    label_score = lv_label_create(ui_game_simon_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "Level: 1");
    lv_obj_align(label_score, LV_ALIGN_TOP_MID, 0, 5);

    // 提示文字
    label_msg = lv_label_create(ui_game_simon_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_msg, "记忆大师\n观察发光顺序并用魔杖复现\n右挥魔杖开始");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 0);

    // 四个方向方块
    const char* labels[4] = {"上", "下", "左", "右"};
    for (int i = 0; i < 4; i++) {
        arrows[i] = lv_obj_create(ui_game_simon_screen);
        lv_obj_set_size(arrows[i], 40, 40);
        lv_obj_set_style_radius(arrows[i], 6, 0);
        lv_obj_set_style_border_width(arrows[i], 2, 0);
        lv_obj_clear_flag(arrows[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * lbl = lv_label_create(arrows[i]);
        lv_obj_set_style_text_font(lbl, &my_font_cn_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(lbl, labels[i]);
    }
    lv_obj_align(arrows[0], LV_ALIGN_CENTER, 0, -60);
    lv_obj_align(arrows[1], LV_ALIGN_CENTER, 0,  60);
    lv_obj_align(arrows[2], LV_ALIGN_CENTER, -60, 0);
    lv_obj_align(arrows[3], LV_ALIGN_CENTER,  60, 0);
    draw_arrows();

    // 暂停菜单
    pause_overlay = lv_obj_create(ui_game_simon_screen);
    lv_obj_set_size(pause_overlay, 160, 160);
    lv_obj_align(pause_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pause_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pause_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(pause_overlay, lv_color_white(), 0);
    lv_obj_set_style_border_width(pause_overlay, 2, 0);
    lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        label_pause_options[i] = lv_label_create(pause_overlay);
        lv_obj_set_style_text_font(label_pause_options[i], &my_font_cn_16, 0);
        lv_obj_align(label_pause_options[i], LV_ALIGN_TOP_MID, 0, 25 + i * 40);
    }

    game_timer = lv_timer_create(simon_tick_cb, 400, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   手势路由
// ==========================================
void game_simon_screen_handle_cmd(ui_cmd_t cmd) {
    // 未开始
    if (game_state == STATE_INIT) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            current_level = 1;
            if (lvgl_port_lock(0)) {
                lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                start_next_level();
                lvgl_port_unlock();
            }
            is_paused = false;
        }
    }
    // 暂停中
    else if (is_paused) {
        if (lvgl_port_lock(0)) {
            switch (cmd) {
                case UI_CMD_UP:
                    pause_choice = (pause_choice - 1 + 3) % 3;
                    update_pause_menu();
                    break;
                case UI_CMD_DOWN:
                    pause_choice = (pause_choice + 1) % 3;
                    update_pause_menu();
                    break;
                case UI_CMD_RIGHT:
                    if (pause_choice == 0) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        if (game_state == STATE_DEMO || game_state == STATE_FLASH) {
                            lv_timer_resume(game_timer);
                        }
                    } else if (pause_choice == 1) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        current_level = 1;
                        start_next_level();
                    } else if (pause_choice == 2) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_set_style_text_color(label_msg, lv_color_white(), 0);
                        lv_label_set_text(label_msg, "记忆大师\n观察发光顺序并用魔杖复现\n右挥魔杖开始");
                        is_paused = false;
                        game_state = STATE_INIT;
                        lv_timer_pause(game_timer);
                        current_lit_arrow = -1;
                        draw_arrows();
                        extern void switch_to_screen(ui_screen_state_t target);
                        switch_to_screen(SCREEN_GAME_LIST);
                    }
                    break;
                case UI_CMD_CIRCLE:
                    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                    is_paused = false;
                    if (game_state == STATE_DEMO || game_state == STATE_FLASH) {
                        lv_timer_resume(game_timer);
                    }
                    break;
                default: break;
            }
            lvgl_port_unlock();
        }
    }
    // 画圆暂停
    else if (cmd == UI_CMD_CIRCLE) {
        if (lvgl_port_lock(0)) {
            lv_timer_pause(game_timer);
            pause_choice = 0;
            update_pause_menu();
            lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
        is_paused = true;
    }
    // 玩家操作
    else if (game_state == STATE_PLAYING) {
        int dir = -1;
        if (cmd == UI_CMD_UP)    dir = 0;
        if (cmd == UI_CMD_DOWN)  dir = 1;
        if (cmd == UI_CMD_LEFT)  dir = 2;
        if (cmd == UI_CMD_RIGHT) dir = 3;

        if (dir != -1) {
            if (lvgl_port_lock(0)) {
                current_lit_arrow = dir;
                draw_arrows();

                if (dir == sequence[player_idx]) {
                    player_idx++;
                    game_state = STATE_FLASH;

                    if (player_idx < current_level) {
                        lv_label_set_text_fmt(label_msg, "正确！继续... (%d/%d)", player_idx, current_level);
                    } else {
                        lv_obj_set_style_text_color(label_msg, lv_color_hex(0xFFD700), 0);
                        lv_label_set_text(label_msg, "完美复现！准备下一关...");
                    }

                    lv_timer_set_period(game_timer, 200);
                    lv_timer_resume(game_timer);
                } else {
                    game_state = STATE_INIT;
                    current_lit_arrow = -1;
                    draw_arrows();

                    lv_obj_set_style_text_color(label_msg, lv_color_hex(0xFF0000), 0);
                    lv_label_set_text_fmt(label_msg, "顺序错啦！\n记忆中断于 Level: %d\n右挥重新挑战", current_level);
                }
                lvgl_port_unlock();
            }
        }
    }
}
