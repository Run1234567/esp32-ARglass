/**
 * @file ui_game_snake.c
 * @brief 赛博贪吃蛇 —— 极简线框体感版
 *
 * 视觉风格：全黑背景，白色边框，高亮青色蛇身，红色食物。
 * 手势操作：
 * 【游玩中】：上/下/左/右挥动改变方向，画圆暂停
 * 【暂停中】：上/下切换选项，右挥/画圆确认
 * 【未开始】：右挥开始，左挥退出
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

// ==========================================
//   游戏参数
// ==========================================
#define BOARD_W     18
#define BOARD_H     18
#define CELL_SIZE   10
#define CANVAS_W    (BOARD_W * CELL_SIZE)
#define CANVAS_H    (BOARD_H * CELL_SIZE)

typedef struct { int x; int y; } Point;
typedef enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT } SnakeDir;

// ==========================================
//   全局对象与状态
// ==========================================
lv_obj_t * ui_game_snake_screen;
static lv_obj_t * snake_canvas;
static lv_obj_t * label_score;
static lv_obj_t * label_msg;
static lv_timer_t * game_timer = NULL;
static lv_color_t *cbuf = NULL;

static lv_obj_t * pause_overlay;
static lv_obj_t * label_pause_options[3];

static Point snake[BOARD_W * BOARD_H];
static int snake_len = 0;
static Point food;
static SnakeDir current_dir;
static SnakeDir next_dir;
static int score = 0;
static bool is_playing = false;
static bool is_paused = false;
static int pause_choice = 0;

// ==========================================
//   核心逻辑
// ==========================================
static void spawn_food(void) {
    bool valid = false;
    while (!valid) {
        food.x = rand() % BOARD_W;
        food.y = rand() % BOARD_H;
        valid = true;
        for (int i = 0; i < snake_len; i++) {
            if (snake[i].x == food.x && snake[i].y == food.y) {
                valid = false;
                break;
            }
        }
    }
}

static void draw_board(void) {
    lv_canvas_fill_bg(snake_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = 1;

    // 食物（红色）
    rect_dsc.bg_color = lv_color_hex(0xFF0000);
    lv_canvas_draw_rect(snake_canvas, food.x * CELL_SIZE, food.y * CELL_SIZE,
                        CELL_SIZE, CELL_SIZE, &rect_dsc);

    // 蛇身（青色）
    rect_dsc.bg_color = lv_color_hex(0x00FFFF);
    for (int i = 0; i < snake_len; i++) {
        lv_canvas_draw_rect(snake_canvas, snake[i].x * CELL_SIZE, snake[i].y * CELL_SIZE,
                            CELL_SIZE, CELL_SIZE, &rect_dsc);
    }

    lv_label_set_text_fmt(label_score, "Score: %d", score);
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

static void reset_game(void) {
    snake_len = 3;
    snake[0].x = BOARD_W / 2;     snake[0].y = BOARD_H / 2;
    snake[1].x = BOARD_W / 2 - 1; snake[1].y = BOARD_H / 2;
    snake[2].x = BOARD_W / 2 - 2; snake[2].y = BOARD_H / 2;
    current_dir = DIR_RIGHT;
    next_dir = DIR_RIGHT;
    score = 0;
}

static void game_tick_cb(lv_timer_t * timer) {
    if (!is_playing || is_paused) return;

    if (lvgl_port_lock(0)) {
        current_dir = next_dir;

        Point new_head = snake[0];
        switch (current_dir) {
            case DIR_UP:    new_head.y--; break;
            case DIR_DOWN:  new_head.y++; break;
            case DIR_LEFT:  new_head.x--; break;
            case DIR_RIGHT: new_head.x++; break;
        }

        // 撞墙
        if (new_head.x < 0 || new_head.x >= BOARD_W ||
            new_head.y < 0 || new_head.y >= BOARD_H) {
            goto game_over;
        }

        // 撞自己
        for (int i = 0; i < snake_len - 1; i++) {
            if (new_head.x == snake[i].x && new_head.y == snake[i].y) {
                goto game_over;
            }
        }

        // 移动
        for (int i = snake_len; i > 0; i--) {
            snake[i] = snake[i - 1];
        }
        snake[0] = new_head;

        // 吃食物
        if (new_head.x == food.x && new_head.y == food.y) {
            snake_len++;
            score += 10;
            spawn_food();
        }

        draw_board();
        lvgl_port_unlock();
        return;

game_over:
        is_playing = false;
        lv_timer_pause(game_timer);
        if (label_msg) {
            lv_label_set_text(label_msg, "GAME OVER!\n右挥重开\n左挥退出");
            lv_obj_clear_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}

void game_snake_pause_timer(void) {
    is_playing = false;
    is_paused = false;
    if (game_timer) lv_timer_pause(game_timer);
}

// ==========================================
//   初始化
// ==========================================
void ui_game_snake_init(void) {
    ui_game_snake_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_snake_screen, lv_color_black(), 0);

    // 得分板
    label_score = lv_label_create(ui_game_snake_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "Score: 0");
    lv_obj_align(label_score, LV_ALIGN_TOP_MID, 0, 5);

    // Canvas（显存分配到 PSRAM）
    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CANVAS_W, CANVAS_H) * sizeof(lv_color_t);
    cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!cbuf) {
        ESP_LOGE("SNAKE", "PSRAM alloc failed, fallback to SRAM");
        cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    snake_canvas = lv_canvas_create(ui_game_snake_screen);
    lv_canvas_set_buffer(snake_canvas, cbuf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_border_color(snake_canvas, lv_color_white(), 0);
    lv_obj_set_style_border_width(snake_canvas, 1, 0);
    lv_obj_align(snake_canvas, LV_ALIGN_CENTER, 0, 10);

    // 提示标签
    label_msg = lv_label_create(ui_game_snake_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_label_set_text(label_msg, "右挥开始\n画圆暂停");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 10);

    // 暂停菜单
    pause_overlay = lv_obj_create(ui_game_snake_screen);
    lv_obj_set_size(pause_overlay, CANVAS_W, CANVAS_H);
    lv_obj_align(pause_overlay, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(pause_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pause_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_color(pause_overlay, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_border_width(pause_overlay, 2, 0);
    lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        label_pause_options[i] = lv_label_create(pause_overlay);
        lv_obj_set_style_text_font(label_pause_options[i], &my_font_cn_16, 0);
        lv_obj_align(label_pause_options[i], LV_ALIGN_TOP_MID, 0, 30 + i * 40);
    }

    // 200ms 一步
    game_timer = lv_timer_create(game_tick_cb, 200, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   手势路由
// ==========================================
void game_snake_screen_handle_cmd(ui_cmd_t cmd) {
    // 未开始
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            reset_game();
            if (lvgl_port_lock(0)) {
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                spawn_food();
                draw_board();
                lvgl_port_unlock();
            }
            is_playing = true;
            is_paused = false;
            lv_timer_resume(game_timer);
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
                        lv_timer_resume(game_timer);
                    } else if (pause_choice == 1) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        reset_game();
                        spawn_food();
                        draw_board();
                        lv_timer_resume(game_timer);
                    } else if (pause_choice == 2) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_clear_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        is_playing = false;
                        extern void switch_to_screen(ui_screen_state_t target);
                        switch_to_screen(SCREEN_GAME_LIST);
                    }
                    break;
                case UI_CMD_CIRCLE:
                    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                    is_paused = false;
                    lv_timer_resume(game_timer);
                    break;
                default: break;
            }
            lvgl_port_unlock();
        }
    }
    // 游戏中
    else {
        if (cmd == UI_CMD_CIRCLE) {
            if (lvgl_port_lock(0)) {
                lv_timer_pause(game_timer);
                pause_choice = 0;
                update_pause_menu();
                lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                lvgl_port_unlock();
            }
            is_paused = true;
        }
        else {
            // 防 180 度掉头
            if (cmd == UI_CMD_UP    && current_dir != DIR_DOWN)  next_dir = DIR_UP;
            if (cmd == UI_CMD_DOWN  && current_dir != DIR_UP)    next_dir = DIR_DOWN;
            if (cmd == UI_CMD_LEFT  && current_dir != DIR_RIGHT) next_dir = DIR_LEFT;
            if (cmd == UI_CMD_RIGHT && current_dir != DIR_LEFT)  next_dir = DIR_RIGHT;
        }
    }
}
