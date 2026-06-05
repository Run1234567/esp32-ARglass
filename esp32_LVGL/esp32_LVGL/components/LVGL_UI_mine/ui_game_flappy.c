#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include <stdlib.h>

// 声明小鸟图片资源
LV_IMG_DECLARE(XSBird);

lv_obj_t * ui_game_flappy_screen;
static lv_obj_t * bird;
static lv_obj_t * pipe_top;
static lv_obj_t * pipe_bottom;
static lv_obj_t * flappy_score_label;
static lv_obj_t * flappy_msg_label;
static lv_timer_t * game_timer = NULL;

// 游戏物理参数 (适配 240x240 屏幕)
#define GRAVITY         1      // 重力加速度（每帧向下加速）
#define JUMP_STRENGTH  -12      // 跳跃力度（负值=向上）
#define PIPE_SPEED      5       // 管道向左移动速度（像素/帧）
#define PIPE_GAP        85      // 上下管道之间的通道间距
#define BIRD_WIDTH      30      // 小鸟图片宽度
#define BIRD_HEIGHT     19      // 小鸟图片高度
#define PIPE_WIDTH      35      // 管道宽度
#define SCREEN_W        240     // 屏幕宽度
#define SCREEN_H        240     // 屏幕高度

static int bird_y = 120;
static int bird_velocity = 0;
static int pipe_x = 240;
static int pipe_gap_y = 120;
static int flappy_score = 0;
static bool flappy_is_playing = false;

// 供外部调用的安全闭合接口
void game_flappy_pause_timer(void) {
    flappy_is_playing = false;
    if (game_timer) lv_timer_pause(game_timer);
}

static void game_over(void) {
    game_flappy_pause_timer();
    lv_label_set_text(flappy_msg_label, "Game Over!\n右挥重开\n左挥退出");
    lv_obj_clear_flag(flappy_msg_label, LV_OBJ_FLAG_HIDDEN);
}

static bool check_collision(void) {
    // 1. 触顶或触底检测
    if (bird_y <= 0 || bird_y + BIRD_HEIGHT >= SCREEN_H) return true;

    // 2. 鸟的左右边界判定
    int bird_right = 50 + BIRD_WIDTH; // 鸟固定在 X=50
    int bird_left = 50;

    // 3. 进入了管道所在的 X 轴区间
    if (bird_right > pipe_x && bird_left < pipe_x + PIPE_WIDTH) {
        // 判断 Y 轴是否撞到了上管道的底部，或下管道的顶部
        if (bird_y < pipe_gap_y - PIPE_GAP/2 || bird_y + BIRD_HEIGHT > pipe_gap_y + PIPE_GAP/2) {
            return true;
        }
    }
    return false;
}

// 物理循环
static void flappy_game_loop(lv_timer_t * timer) {
    if (!flappy_is_playing) return;

    if (lvgl_port_lock(0)) {
        bird_velocity += GRAVITY;
        bird_y += bird_velocity;
        lv_obj_set_y(bird, bird_y);

        pipe_x -= PIPE_SPEED;
        if (pipe_x < -PIPE_WIDTH) {
            pipe_x = SCREEN_W;
            pipe_gap_y = 60 + (rand() % 120);
            flappy_score++;
            lv_label_set_text_fmt(flappy_score_label, "得分: %d", flappy_score);
        }

        lv_obj_set_pos(pipe_top, pipe_x, 0);
        lv_obj_set_size(pipe_top, PIPE_WIDTH, pipe_gap_y - PIPE_GAP/2);

        lv_obj_set_pos(pipe_bottom, pipe_x, pipe_gap_y + PIPE_GAP/2);
        lv_obj_set_size(pipe_bottom, PIPE_WIDTH, SCREEN_H - (pipe_gap_y + PIPE_GAP/2));

        if (check_collision()) {
            game_over();
        }
        lvgl_port_unlock();
    }
}

// 初始化（纯白底、黑线框极简风）
void ui_game_flappy_init(void) {
    ui_game_flappy_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_flappy_screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(ui_game_flappy_screen, 0, 0);

    flappy_score_label = lv_label_create(ui_game_flappy_screen);
    lv_obj_set_style_text_color(flappy_score_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(flappy_score_label, &my_font_cn_16, 0);
    lv_label_set_text(flappy_score_label, "得分: 0");
    lv_obj_align(flappy_score_label, LV_ALIGN_TOP_MID, 0, 15);

    // 极简像素方块鸟
    bird = lv_img_create(ui_game_flappy_screen);
    lv_img_set_src(bird, &XSBird);
    lv_obj_set_pos(bird, 50, bird_y);
    lv_obj_set_style_bg_opa(bird, 0, 0);
    lv_obj_set_style_border_width(bird, 0, 0);

    // 几何障碍管道
    pipe_top = lv_obj_create(ui_game_flappy_screen);
    lv_obj_set_style_bg_color(pipe_top, lv_color_white(), 0);
    lv_obj_set_style_border_color(pipe_top, lv_color_black(), 0);
    lv_obj_set_style_border_width(pipe_top, 2, 0);
    lv_obj_set_style_radius(pipe_top, 0, 0);
    lv_obj_set_style_pad_all(pipe_top, 0, 0);

    pipe_bottom = lv_obj_create(ui_game_flappy_screen);
    lv_obj_set_style_bg_color(pipe_bottom, lv_color_white(), 0);
    lv_obj_set_style_border_color(pipe_bottom, lv_color_black(), 0);
    lv_obj_set_style_border_width(pipe_bottom, 2, 0);
    lv_obj_set_style_radius(pipe_bottom, 0, 0);
    lv_obj_set_style_pad_all(pipe_bottom, 0, 0);

    flappy_msg_label = lv_label_create(ui_game_flappy_screen);
    lv_obj_set_style_text_color(flappy_msg_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(flappy_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(flappy_msg_label, &my_font_cn_16, 0);
    lv_label_set_text(flappy_msg_label, "右挥启动战局\n上挥控制跳跃");
    lv_obj_align(flappy_msg_label, LV_ALIGN_CENTER, 0, 0);

    game_timer = lv_timer_create(flappy_game_loop, 33, NULL); // 约30FPS
    lv_timer_pause(game_timer);
}

// 独占手势路由
void game_flappy_screen_handle_cmd(ui_cmd_t cmd) {
    if (!flappy_is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            bird_y = 100;
            bird_velocity = 0;
            pipe_x = SCREEN_W;
            flappy_score = 0;

            lv_obj_add_flag(flappy_msg_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(flappy_score_label, "得分: 0");
            lv_obj_set_pos(pipe_top, SCREEN_W, 0);
            lv_obj_set_pos(pipe_bottom, SCREEN_W, SCREEN_H);

            flappy_is_playing = true;
            lv_timer_resume(game_timer);
        }
    }
    else {
        if (cmd == UI_CMD_UP) {
            bird_velocity = JUMP_STRENGTH;
        }
        else if (cmd == UI_CMD_LEFT) {
            game_flappy_pause_timer();
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
    }
}
