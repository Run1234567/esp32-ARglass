#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "my_uart.h"
#include "esp_log.h"
#include <stdio.h>

// 火箭图像声明
LV_IMG_DECLARE(Rocket);

// ==========================================
//   跑酷游戏全局对象与参数
// ==========================================
lv_obj_t  * ui_game_screen;
static lv_obj_t  * player;
static lv_obj_t  * obstacle;
static lv_obj_t  * label_score;
static lv_obj_t  * label_msg;
static lv_timer_t  * game_timer;

// 物理与状态参数
static int player_y = 160;       // 玩家初始高度
static float velocity_y = 0;     // 垂直速度
static float gravity = 0.4;      // 重力加速度
static float jump_force = -10.0; // 跳跃力度

static int obstacle_x = 240;     // 障碍物初始X位置
static int obstacle_speed = 3;   // 障碍物移动速度

static int score = 0;
static bool is_playing = false;

// 固定尺寸定义
#define GROUND_Y 190
#define PLAYER_SIZE 30
#define PLAYER_FIXED_X 40
#define OBS_WIDTH 50
#define OBS_HEIGHT 17

// ==========================================
//   游戏主循环 (20ms 刷新一次 = 50FPS)
// ==========================================
static void game_loop_cb(lv_timer_t * timer) {
    if (!is_playing) return;

    // ⚠️ 因为这是定时器回调，操作 UI 必须上锁！
    if (lvgl_port_lock(0)) {
        
        // 1. 【物理引擎】：更新玩家位置
        velocity_y += gravity;
        player_y += (int)velocity_y;

        // 地面碰撞检测 (不让玩家掉下去)
        if (player_y >= GROUND_Y - PLAYER_SIZE) {
            player_y = GROUND_Y - PLAYER_SIZE;
            velocity_y = 0;
        }

        // 2. 【世界运转】：障碍物向左移动
        obstacle_x -= obstacle_speed;
        
        // 障碍物出界，重置位置并加分
        if (obstacle_x < -OBS_WIDTH) {
            obstacle_x = 240;
            score++;
            lv_label_set_text_fmt(label_score, "Score: %d", score);
            
            // 随着分数增加，微微加速增加难度！
            if (score % 5 == 0 && obstacle_speed < 18) {
                obstacle_speed += 1;
            }
        }

        // 刷新坐标到屏幕
        lv_obj_set_y(player, player_y);
        // 让火箭保持悬浮高度 10 像素
        lv_obj_set_pos(obstacle, obstacle_x, GROUND_Y - OBS_HEIGHT - 10);

        // 3. 【生死判定】：AABB 碰撞检测
        bool collision_x = (PLAYER_FIXED_X < obstacle_x + OBS_WIDTH) && (PLAYER_FIXED_X + PLAYER_SIZE > obstacle_x);
        bool collision_y = (player_y < GROUND_Y) && (player_y + PLAYER_SIZE > GROUND_Y - OBS_HEIGHT);

        if (collision_x && collision_y) {
            is_playing = false;
            lv_label_set_text_fmt(label_msg, "GAME OVER\nScore: %d\nSwipe Right to Restart", score);
            lv_obj_clear_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
        }

        lvgl_port_unlock();
    }
}

// ==========================================
//   初始化游戏界面
// ==========================================
void ui_game_screen_init(void) {
    ui_game_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_screen, lv_color_hex(0x111111), 0);

    // 1. 地面装饰线
    lv_obj_t * ground_line = lv_obj_create(ui_game_screen);
    lv_obj_set_size(ground_line, 240, 2);
    lv_obj_set_pos(ground_line, 0, GROUND_Y);
    lv_obj_set_style_bg_color(ground_line, lv_color_white(), 0);
    lv_obj_set_style_border_width(ground_line, 0, 0);

    // 2. 玩家 (一个发光的蓝色方块)
    player = lv_obj_create(ui_game_screen);
    lv_obj_set_size(player, PLAYER_SIZE, PLAYER_SIZE);
    lv_obj_set_pos(player, PLAYER_FIXED_X, GROUND_Y - PLAYER_SIZE);
    lv_obj_set_style_bg_color(player, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_radius(player, 5, 0);
    lv_obj_set_style_border_width(player, 0, 0);

    // 3. 障碍物 (换成炫酷的火箭/导弹图片！)
    obstacle = lv_img_create(ui_game_screen);
    lv_img_set_src(obstacle, &Rocket);
    lv_obj_set_pos(obstacle, obstacle_x, GROUND_Y - OBS_HEIGHT - 10);

    // 4. 分数标签
    label_score = lv_label_create(ui_game_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "Score: 0");
    lv_obj_align(label_score, LV_ALIGN_TOP_MID, 0, 10);

    // 5. 状态提示文本
    label_msg = lv_label_create(ui_game_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_msg, "Swipe Right to START");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, -30);

    // 6. 创建高频游戏定时器 (先暂停)
    game_timer = lv_timer_create(game_loop_cb, 20, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   游戏专属手势控制路由
// ==========================================
void game_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT) {
        // 退出游戏：暂停定时器，退回游戏列表
        is_playing = false;
        lv_timer_pause(game_timer);
        
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_GAME_LIST);
    }
    else if (cmd == UI_CMD_RIGHT) {
        // 开始 / 重启游戏
        if (!is_playing) {
            player_y = GROUND_Y - PLAYER_SIZE;
            velocity_y = 0;
            obstacle_x = 240;
            obstacle_speed = 3;
            score = 0;
            
            if (lvgl_port_lock(0)) {
                lv_label_set_text(label_score, "Score: 0");
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(player, player_y);
                lv_obj_set_x(obstacle, obstacle_x);
                lvgl_port_unlock();
            }
            
            is_playing = true;
            lv_timer_resume(game_timer);
        }
    }
    else if (cmd == UI_CMD_UP) {
        // 只有当玩家踩在地上时，才允许跳跃！(防止空中连跳)
        if (is_playing && player_y >= GROUND_Y - PLAYER_SIZE) {
            velocity_y = jump_force;
        }
    }
}
