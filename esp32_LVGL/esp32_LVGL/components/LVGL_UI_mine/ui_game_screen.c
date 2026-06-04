#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "my_uart.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h> // ✨ 新增：用于随机数生成

// 火箭和背景图像声明
LV_IMG_DECLARE(Rocket);
LV_IMG_DECLARE(beijing);
LV_IMG_DECLARE(people1);
LV_IMG_DECLARE(people1_d); // ✨ 新增：下蹲角色图像声明

// ==========================================
//   跑酷游戏全局对象与参数
// ==========================================
lv_obj_t  * ui_game_screen;
static lv_obj_t  * player;
static lv_obj_t  * obstacle;
static lv_obj_t  * label_score;
static lv_obj_t  * label_msg;
static lv_timer_t  * game_timer;

// 物理与状态参数 (优化适配 30FPS 畅玩帧率)
static int player_y = 142;       // 玩家初始高度
static float velocity_y = 0;     // 垂直速度
static float gravity = 0.8;      // 重力加速度
static float jump_force = -14.0; // 跳跃力度

// 下蹲状态变量
static bool is_ducking = false;  // ✨ 是否正在下蹲
static int duck_timer = 0;       // ✨ 下蹲倒计时器

static int obstacle_x = 240;     // 障碍物初始X位置
static int obstacle_y = 168;     // ✨ 障碍物动态Y位置
static int obstacle_speed = 5;   // 障碍物移动速度
static int obstacle_type = 0;    // ✨ 0: 低空导弹(跳跃避开), 1: 高空导弹(下蹲避开)

// 背景滚动变量
static lv_obj_t * bg_img1;
static lv_obj_t * bg_img2;
static float bg_x1 = 0;
static float bg_x2 = 480;
static float bg_speed = 2.0; 

static int score = 0;
static bool is_playing = false;

// 固定尺寸定义
#define GROUND_Y 190
#define PLAYER_WIDTH 50
#define PLAYER_HEIGHT 48
#define PLAYER_DUCK_WIDTH 60    // ✨ 下蹲宽度
#define PLAYER_DUCK_HEIGHT 20   // ✨ 下蹲高度
#define PLAYER_FIXED_X 40
#define OBS_WIDTH 50
#define OBS_HEIGHT 17
#define BG_WIDTH 480
#define DUCK_DURATION_FRAMES 24 // ✨ 下蹲持续帧数 (24帧约等于 800ms)

// ==========================================
//   游戏主循环 (33ms 刷新一次 = 30FPS 畅玩)
// ==========================================
static void game_loop_cb(lv_timer_t * timer) {
    if (!is_playing) return;

    if (lvgl_port_lock(0)) {
        
        // 1. 【状态控制】：处理下蹲计时自动站起
        if (is_ducking) {
            duck_timer--;
            player_y = GROUND_Y - PLAYER_DUCK_HEIGHT; // 强制保持在下蹲地面高度
            velocity_y = 0;
            if (duck_timer <= 0) {
                is_ducking = false;
                lv_img_set_src(player, &people1); // ✨ 变回站立贴图
                player_y = GROUND_Y - PLAYER_HEIGHT;
            }
        } else {
            // 【物理引擎】：未下蹲时更新跳跃位置
            velocity_y += gravity;
            player_y += (int)velocity_y;

            // 地面碰撞检测
            if (player_y >= GROUND_Y - PLAYER_HEIGHT) {
                player_y = GROUND_Y - PLAYER_HEIGHT;
                velocity_y = 0;
            }
        }

        // 2. 【世界运转】：障碍物向左移动
        obstacle_x -= obstacle_speed;
        
        // 障碍物出界，重置位置、刷新类型并加分
        if (obstacle_x < -OBS_WIDTH) {
            obstacle_x = 240;
            
            // ✨ 核心逻辑：随机生成导弹类型 (50% 概率高空或低空)
            obstacle_type = rand() % 2; 
            if (obstacle_type == 0) {
                // 低空导弹：擦着地面飞，需要起跳
                obstacle_y = GROUND_Y - OBS_HEIGHT - 5; 
            } else {
                // 高空导弹：精准瞄准方块头部，必须下蹲
                obstacle_y = GROUND_Y - PLAYER_HEIGHT + 5; 
            }

            score++;
            lv_label_set_text_fmt(label_score, "当前得分: %d", score);
            
            // 随着分数增加难度上升
            if (score % 5 == 0 && obstacle_speed < 18) {
                obstacle_speed += 1;
            }
        }

        // 3. 【视差卷轴】：背景长图缓慢移动
        bg_x1 -= bg_speed;
        bg_x2 -= bg_speed;

        if (bg_x1 <= -BG_WIDTH) bg_x1 = bg_x2 + BG_WIDTH;
        if (bg_x2 <= -BG_WIDTH) bg_x2 = bg_x1 + BG_WIDTH;

        // 刷新所有物体的坐标到显示屏
        lv_obj_set_y(player, player_y);
        lv_obj_set_pos(obstacle, obstacle_x, obstacle_y); // ✨ 使用动态 Y 轴坐标
        lv_obj_set_x(bg_img1, (int)bg_x1);
        lv_obj_set_x(bg_img2, (int)bg_x2);

        // 4. 【生死判定】：动态 Z 轴 AABB 碰撞检测
        int current_w = is_ducking ? PLAYER_DUCK_WIDTH : PLAYER_WIDTH;
        int current_h = is_ducking ? PLAYER_DUCK_HEIGHT : PLAYER_HEIGHT;

        bool collision_x = (PLAYER_FIXED_X < obstacle_x + OBS_WIDTH) && (PLAYER_FIXED_X + current_w > obstacle_x);
        bool collision_y = (player_y < obstacle_y + OBS_HEIGHT) && (player_y + current_h > obstacle_y); // ✨ 完美匹配高低空碰撞

        if (collision_x && collision_y) {
            is_playing = false;
            lv_label_set_text_fmt(label_msg, "游戏结束\n最终得分: %d\n👉 右挥重新开始", score);
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
    lv_obj_set_style_bg_color(ui_game_screen, lv_color_black(), 0);

    // 1. 创建两张长背景图 (必须在最底层)
    bg_img1 = lv_img_create(ui_game_screen);
    lv_img_set_src(bg_img1, &beijing);
    lv_obj_set_pos(bg_img1, (int)bg_x1, 0);

    bg_img2 = lv_img_create(ui_game_screen);
    lv_img_set_src(bg_img2, &beijing);
    lv_obj_set_pos(bg_img2, (int)bg_x2, 0);

    // 2. 地面装饰线
    lv_obj_t * ground_line = lv_obj_create(ui_game_screen);
    lv_obj_set_size(ground_line, 240, 2);
    lv_obj_set_pos(ground_line, 0, GROUND_Y);
    lv_obj_set_style_bg_color(ground_line, lv_color_white(), 0);
    lv_obj_set_style_border_width(ground_line, 0, 0);

    // 3. 玩家 (初始为站立状态贴图)
    player = lv_img_create(ui_game_screen);
    lv_img_set_src(player, &people1);
    lv_obj_set_style_bg_opa(player, 0, 0);
    lv_obj_set_style_border_width(player, 0, 0);
    lv_obj_set_pos(player, PLAYER_FIXED_X, GROUND_Y - PLAYER_HEIGHT);

    // 4. 障碍物
    obstacle = lv_img_create(ui_game_screen);
    lv_img_set_src(obstacle, &Rocket);
    obstacle_y = GROUND_Y - OBS_HEIGHT - 5; // 默认第一发是低空
    lv_obj_set_pos(obstacle, obstacle_x, obstacle_y);

    // 5. 分数标签
    label_score = lv_label_create(ui_game_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "当前得分: 0");
    lv_obj_align(label_score, LV_ALIGN_TOP_MID, 0, 10);

    // 6. 状态提示文本
    label_msg = lv_label_create(ui_game_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_label_set_text(label_msg, "👉 右挥魔杖开始游戏");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, -30);

    // 7. 创建游戏定时器 (33ms = 30FPS 减负抗卡顿)
    game_timer = lv_timer_create(game_loop_cb, 33, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   游戏手势控制核心路由
// ==========================================
void game_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT) {
        is_playing = false;
        lv_timer_pause(game_timer);
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_GAME_LIST);
    }
    else if (cmd == UI_CMD_RIGHT) {
        // 开始 / 重启游戏
        if (!is_playing) {
            is_ducking = false;
            duck_timer = 0;
            player_y = GROUND_Y - PLAYER_HEIGHT;
            velocity_y = 0;
            obstacle_x = 240;
            obstacle_y = GROUND_Y - OBS_HEIGHT - 5;
            obstacle_speed = 5;
            score = 0;
            
            if (lvgl_port_lock(0)) {
                lv_img_set_src(player, &people1); // 确保恢复站立贴图
                lv_label_set_text(label_score, "当前得分: 0");
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(player, player_y);
                lv_obj_set_pos(obstacle, obstacle_x, obstacle_y);
                lvgl_port_unlock();
            }
            
            is_playing = true;
            lv_timer_resume(game_timer);
        }
    }
    else if (cmd == UI_CMD_UP) {
        // 🚀 起跳：只有在地上且没在下蹲时才允许
        if (is_playing && !is_ducking && player_y >= GROUND_Y - PLAYER_HEIGHT) {
            velocity_y = jump_force;
        }
    }
    else if (cmd == UI_CMD_DOWN) {
        // 🧎 下蹲：只有在地上且没在下蹲时才允许触发
        if (is_playing && !is_ducking && player_y >= GROUND_Y - PLAYER_HEIGHT) {
            is_ducking = true;
            duck_timer = DUCK_DURATION_FRAMES; // 开启下蹲倒计时
            
            if (lvgl_port_lock(0)) {
                lv_img_set_src(player, &people1_d); // ✨ 切换为下蹲动作贴图！
                player_y = GROUND_Y - PLAYER_DUCK_HEIGHT; // 修正 Y 轴坐标
                lv_obj_set_y(player, player_y);
                lvgl_port_unlock();
            }
        }
    }
}