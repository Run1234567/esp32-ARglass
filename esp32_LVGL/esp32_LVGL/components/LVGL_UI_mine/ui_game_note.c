#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include <stdlib.h>

// 声明树木图片资源
LV_IMG_DECLARE(tree);

lv_obj_t * ui_game_note_screen;
static lv_obj_t * player;          // "音符君"（一个小方块）
static lv_obj_t * obstacle;        // 地面的障碍物
static lv_obj_t * floor_line;      // 地面线
static lv_obj_t * score_label;     // 计分
static lv_obj_t * msg_label;       // 提示状态
static lv_obj_t * db_indicator;    // 屏幕顶部分贝可视化能量条
static lv_timer_t * note_timer = NULL;

#define SCREEN_W        240     // 屏幕宽度
#define SCREEN_H        240     // 屏幕高度
#define FLOOR_Y         180     // 地平线高度
#define PLAYER_SIZE     16      // 玩家方块边长

// 障碍物尺寸精准匹配为树木的真实像素宽高
#define OBS_WIDTH       32      // 树的宽度
#define OBS_HEIGHT      26      // 树的高度

// 声控核心阈值（可根据麦克风灵敏度自行微调）
#define DB_SILENCE      55      // 低于此分贝判定为沉默，走不动
#define DB_JUMP         72      // 高于此分贝判定为尖叫，触发跳跃

static int player_y = FLOOR_Y - PLAYER_SIZE;
static int player_vel_y = 0;
static int obs_x = SCREEN_W;
static int game_score = 0;
static int current_db = 0;
static bool is_playing = false;
static bool is_grounded = true;

// 供串口调用的高频分贝注入函数
void game_note_pass_db(int val) {
    current_db = val;
    // 实时核心：如果在大叫，且人在地面，立刻触发起跳防延迟！
    if (is_playing && val >= DB_JUMP && is_grounded) {
        player_vel_y = -12;        // 赋予向上的跳跃速度
        is_grounded = false;
    }
}

static void game_over(void) {
    is_playing = false;
    if (note_timer) lv_timer_pause(note_timer);
    lv_label_set_text(msg_label, "游戏结束!\n右挥重新开始\n左挥退出");
    lv_obj_clear_flag(msg_label, LV_OBJ_FLAG_HIDDEN);
}

// 矩形碰撞检测
static bool check_collision(void) {
    int p_left = 50, p_right = 50 + PLAYER_SIZE;
    int p_top = player_y, p_bottom = player_y + PLAYER_SIZE;

    int o_left = obs_x, o_right = obs_x + OBS_WIDTH;
    int o_top = FLOOR_Y - OBS_HEIGHT, o_bottom = FLOOR_Y;

    if (p_right > o_left && p_left < o_right) {
        if (p_bottom > o_top && p_top < o_bottom) {
            return true;
        }
    }
    return false;
}

// 游戏实时物理循环（约30帧）
static void note_game_loop(lv_timer_t * timer) {
    if (!is_playing) return;

    if (lvgl_port_lock(0)) {
        // 1. 刷新顶部分贝视觉反馈条
        int bar_w = (current_db > 40) ? (current_db - 40) * 2 : 0;
        lv_obj_set_size(db_indicator, bar_w, 4);

        // 2. 垂直方向物理引擎（重力机制）
        if (!is_grounded) {
            player_vel_y += 1;     // 重力加速度
            player_y += player_vel_y;

            if (player_y >= FLOOR_Y - PLAYER_SIZE) { // 落回地面
                player_y = FLOOR_Y - PLAYER_SIZE;
                player_vel_y = 0;
                is_grounded = true;
            }
            lv_obj_set_y(player, player_y);
        }

        // 3. 水平方向滚动速度控制（声控算法）
        int scroll_speed = 0;
        if (current_db > DB_SILENCE) {
            if (current_db >= DB_JUMP) {
                scroll_speed = 6;  // 尖叫时：不仅在跳，地面也在狂奔
            } else {
                scroll_speed = 3;  // 轻哼时：小步漫跑
            }
        } else {
            scroll_speed = 0;      // 沉默时：静止在原地
        }

        // 4. 地面障碍物移动
        if (scroll_speed > 0) {
            obs_x -= scroll_speed;
            if (obs_x < -OBS_WIDTH) {
                obs_x = SCREEN_W + (rand() % 40); // 移出屏幕后重新在右侧生成
                game_score++;
                lv_label_set_text_fmt(score_label, "得分: %d", game_score);
            }
            lv_obj_set_x(obstacle, obs_x);
        }

        // 5. 碰撞判定
        if (check_collision()) {
            game_over();
        }

        lvgl_port_unlock();
    }
}

void ui_game_note_init(void) {
    ui_game_note_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_note_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(ui_game_note_screen, 0, 0);

    // 分贝状态指示器（顶部能量条）
    db_indicator = lv_obj_create(ui_game_note_screen);
    lv_obj_set_pos(db_indicator, 0, 0);
    lv_obj_set_size(db_indicator, 0, 4);
    lv_obj_set_style_bg_color(db_indicator, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(db_indicator, 0, 0);
    lv_obj_set_style_pad_all(db_indicator, 0, 0);

    // 计分板
    score_label = lv_label_create(ui_game_note_screen);
    lv_obj_set_style_text_color(score_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(score_label, &my_font_cn_16, 0);
    lv_label_set_text(score_label, "得分: 0");
    lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 15);

    // 地平线
    floor_line = lv_obj_create(ui_game_note_screen);
    lv_obj_set_size(floor_line, SCREEN_W, 2);
    lv_obj_set_pos(floor_line, 0, FLOOR_Y);
    lv_obj_set_style_bg_color(floor_line, lv_color_white(), 0);
    lv_obj_set_style_border_width(floor_line, 0, 0);
    lv_obj_set_style_pad_all(floor_line, 0, 0);

    // 玩家小方块（音符君）
    player = lv_obj_create(ui_game_note_screen);
    lv_obj_set_size(player, PLAYER_SIZE, PLAYER_SIZE);
    lv_obj_set_pos(player, 50, player_y);
    lv_obj_set_style_bg_color(player, lv_color_white(), 0);
    lv_obj_set_style_border_width(player, 0, 0);
    lv_obj_set_style_radius(player, 2, 0);
    lv_obj_set_style_pad_all(player, 0, 0);

    // 障碍物（替换为树木贴图）
    obstacle = lv_img_create(ui_game_note_screen);
    lv_img_set_src(obstacle, &tree);
    lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT); // 底部紧贴地平线
    lv_obj_set_style_bg_opa(obstacle, 0, 0);               // 确保背景透明

    // 提示语
    msg_label = lv_label_create(ui_game_note_screen);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_label, &my_font_cn_16, 0);
    lv_label_set_text(msg_label, "右挥启动声控\n声音低: 前进\n尖叫: 跳跃");
    lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, -10);

    note_timer = lv_timer_create(note_game_loop, 33, NULL); // 约30FPS
    lv_timer_pause(note_timer);
}

void game_note_screen_handle_cmd(ui_cmd_t cmd) {
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) { // 重置并开启游戏
            player_y = FLOOR_Y - PLAYER_SIZE;
            player_vel_y = 0;
            obs_x = SCREEN_W;
            game_score = 0;
            is_grounded = true;
            current_db = 0;

            lv_obj_add_flag(msg_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(score_label, "得分: 0");
            lv_obj_set_pos(player, 50, player_y);
            lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT);

            is_playing = true;
            lv_timer_resume(note_timer);
        }
    } else {
        if (cmd == UI_CMD_LEFT) { // 玩的过程中允许左挥强退
            is_playing = false;
            lv_timer_pause(note_timer);
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
    }
}
