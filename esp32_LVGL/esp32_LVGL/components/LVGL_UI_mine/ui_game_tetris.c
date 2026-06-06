/**
 * @file ui_game_tetris.c
 * @brief 俄罗斯方块 —— Canvas 高刷渲染版
 *
 * 手势操作：
 *   左挥 → 左移   右挥 → 右移
 *   下挥 → 加速下落  上挥 → 旋转
 *   左挥（游戏中） → 退出
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

// ============================================================
//   游戏参数
// ============================================================
#define BOARD_W     10      // 游戏区宽度（列）
#define BOARD_H     20      // 游戏区高度（行）
#define BLOCK_SIZE  10      // 每个小方块的像素边长
#define CANVAS_W    (BOARD_W * BLOCK_SIZE)  // 画布宽 100px
#define CANVAS_H    (BOARD_H * BLOCK_SIZE)  // 画布高 200px

// ============================================================
//   全局对象
// ============================================================
lv_obj_t * ui_game_tetris_screen;
static lv_obj_t * tetris_canvas;
static lv_obj_t * score_label;
static lv_obj_t * msg_label;
static lv_timer_t * game_timer = NULL;

// 画布显存池（放在 PSRAM 中，节省内部 SRAM）
static lv_color_t *cbuf = NULL;

// 游戏状态
static uint8_t board[BOARD_H][BOARD_W] = {0};
static int current_x = 3;
static int current_y = 0;
static uint8_t current_piece[4][4] = {0};
static int score = 0;
static bool is_playing = false;

// 7 种经典方块矩阵 (4x4)
static const uint8_t TETROMINOES[7][4][4] = {
    {{0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0}}, // I 青色
    {{0,2,2,0}, {0,2,2,0}, {0,0,0,0}, {0,0,0,0}}, // O 黄色
    {{0,3,3,0}, {3,3,0,0}, {0,0,0,0}, {0,0,0,0}}, // S 绿色
    {{4,4,0,0}, {0,4,4,0}, {0,0,0,0}, {0,0,0,0}}, // Z 红色
    {{0,5,0,0}, {5,5,5,0}, {0,0,0,0}, {0,0,0,0}}, // T 紫色
    {{0,0,6,0}, {6,6,6,0}, {0,0,0,0}, {0,0,0,0}}, // L 橙色
    {{7,0,0,0}, {7,7,7,0}, {0,0,0,0}, {0,0,0,0}}  // J 蓝色
};

// 色板
static lv_color_t get_block_color(int val) {
    switch(val) {
        case 1: return lv_color_hex(0x00FFFF); // 青色 I
        case 2: return lv_color_hex(0xFFFF00); // 黄色 O
        case 3: return lv_color_hex(0x00FF00); // 绿色 S
        case 4: return lv_color_hex(0xFF0000); // 红色 Z
        case 5: return lv_color_hex(0xFF00FF); // 紫色 T
        case 6: return lv_color_hex(0xFF8800); // 橙色 L
        case 7: return lv_color_hex(0x0000FF); // 蓝色 J
        default: return lv_color_black();
    }
}

// ============================================================
//   核心物理引擎
// ============================================================

// 生成新方块
static void spawn_new_piece(void) {
    int shape = rand() % 7;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            current_piece[y][x] = TETROMINOES[shape][y][x];
    current_x = 3;
    current_y = 0;
}

// 碰撞检测：返回 1 = 撞了，0 = 安全
static int check_collision(int next_x, int next_y, uint8_t temp[4][4]) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (temp[y][x] != 0) {
                int bx = next_x + x;
                int by = next_y + y;
                if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) return 1;
                if (by >= 0 && board[by][bx] != 0) return 1;
            }
        }
    }
    return 0;
}

// 方块固化到背景
static void merge_piece(void) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (current_piece[y][x] != 0 && current_y + y >= 0)
                board[current_y + y][current_x + x] = current_piece[y][x];
}

// 消除满行
static void clear_lines(void) {
    int cleared = 0;
    for (int y = BOARD_H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BOARD_W; x++) {
            if (board[y][x] == 0) { full = 0; break; }
        }
        if (full) {
            cleared++;
            for (int my = y; my > 0; my--)
                for (int x = 0; x < BOARD_W; x++)
                    board[my][x] = board[my - 1][x];
            for (int x = 0; x < BOARD_W; x++) board[0][x] = 0;
            y++; // 再检查一次当前行
        }
    }
    if (cleared > 0) {
        score += cleared * 100;
        if (score_label) lv_label_set_text_fmt(score_label, "Score: %d", score);
    }
}

// ============================================================
//   Canvas 渲染
// ============================================================
static void draw_game_screen(void) {
    lv_canvas_fill_bg(tetris_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = 2;
    rect_dsc.border_width = 1;
    rect_dsc.border_color = lv_color_hex(0x333333);

    // 画已固定的方块
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            if (board[y][x] != 0) {
                rect_dsc.bg_color = get_block_color(board[y][x]);
                lv_canvas_draw_rect(tetris_canvas,
                    x * BLOCK_SIZE, y * BLOCK_SIZE,
                    BLOCK_SIZE, BLOCK_SIZE, &rect_dsc);
            }
        }
    }

    // 画正在下落的方块
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (current_piece[y][x] != 0) {
                rect_dsc.bg_color = get_block_color(current_piece[y][x]);
                lv_canvas_draw_rect(tetris_canvas,
                    (current_x + x) * BLOCK_SIZE,
                    (current_y + y) * BLOCK_SIZE,
                    BLOCK_SIZE, BLOCK_SIZE, &rect_dsc);
            }
        }
    }
}

// ============================================================
//   游戏心跳（定时器回调）
// ============================================================
static void game_tick_cb(lv_timer_t * timer) {
    if (!is_playing) return;

    if (lvgl_port_lock(0)) {
        if (check_collision(current_x, current_y + 1, current_piece) == 0) {
            current_y++; // 安全，继续下落
        } else {
            merge_piece();
            clear_lines();
            spawn_new_piece();

            // 新方块出生就碰撞 → 游戏结束
            if (check_collision(current_x, current_y, current_piece) != 0) {
                is_playing = false;
                lv_timer_pause(game_timer);
                if (msg_label) {
                    lv_label_set_text(msg_label, "GAME OVER!\n右挥重开\n左挥退出");
                    lv_obj_clear_flag(msg_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        draw_game_screen();
        lvgl_port_unlock();
    }
}

// ============================================================
//   外部操作 API
// ============================================================
static void tetris_move_left(void) {
    if (check_collision(current_x - 1, current_y, current_piece) == 0) {
        current_x--;
        draw_game_screen();
    }
}

static void tetris_move_right(void) {
    if (check_collision(current_x + 1, current_y, current_piece) == 0) {
        current_x++;
        draw_game_screen();
    }
}

static void tetris_move_down(void) {
    if (check_collision(current_x, current_y + 1, current_piece) == 0) {
        current_y++;
        draw_game_screen();
    }
}

static void tetris_rotate(void) {
    uint8_t temp[4][4] = {0};
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            temp[x][3 - y] = current_piece[y][x];

    if (check_collision(current_x, current_y, temp) == 0) {
        memcpy(current_piece, temp, sizeof(temp));
        draw_game_screen();
    }
}

// 安全暂停（切出屏幕时调用）
void game_tetris_pause_timer(void) {
    is_playing = false;
    if (game_timer) lv_timer_pause(game_timer);
}

// ============================================================
//   初始化
// ============================================================
void ui_game_tetris_init(void) {
    ui_game_tetris_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_tetris_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(ui_game_tetris_screen, 0, 0);

    // 分数标签
    score_label = lv_label_create(ui_game_tetris_screen);
    lv_obj_set_style_text_color(score_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(score_label, &my_font_cn_16, 0);
    lv_label_set_text(score_label, "Score: 0");
    lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 5);

    // Canvas 画布（显存分配到 PSRAM，释放内部 SRAM）
    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(CANVAS_W, CANVAS_H) * sizeof(lv_color_t);
    cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!cbuf) {
        ESP_LOGE("TETRIS", "PSRAM 分配失败，回退到内部 SRAM");
        cbuf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
    }
    tetris_canvas = lv_canvas_create(ui_game_tetris_screen);
    lv_canvas_set_buffer(tetris_canvas, cbuf, CANVAS_W, CANVAS_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(tetris_canvas, LV_ALIGN_CENTER, 0, 10);

    // 提示语
    msg_label = lv_label_create(ui_game_tetris_screen);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_label, &my_font_cn_16, 0);
    lv_label_set_text(msg_label, "右挥开始\n上挥旋转\n下挥加速");
    lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, 0);

    // 定时器（600ms 落一格）
    game_timer = lv_timer_create(game_tick_cb, 600, NULL);
    lv_timer_pause(game_timer);
}

// ============================================================
//   手势处理
// ============================================================
void game_tetris_screen_handle_cmd(ui_cmd_t cmd) {
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            // 重置并开始
            memset(board, 0, sizeof(board));
            score = 0;
            current_x = 3;
            current_y = 0;

            if (score_label) lv_label_set_text(score_label, "Score: 0");
            if (msg_label) lv_obj_add_flag(msg_label, LV_OBJ_FLAG_HIDDEN);

            spawn_new_piece();
            draw_game_screen();

            is_playing = true;
            lv_timer_resume(game_timer);
        }
    } else {
        // 游戏中：上/下/左/右控制方块，无退出手势（方块不能丢）
        if (lvgl_port_lock(0)) {
            switch (cmd) {
                case UI_CMD_LEFT:   tetris_move_left();  break;
                case UI_CMD_RIGHT:  tetris_move_right(); break;
                case UI_CMD_DOWN:   tetris_move_down();  break;
                case UI_CMD_UP:     tetris_rotate();     break;
                default: break;
            }
            lvgl_port_unlock();
        }
    }
}
