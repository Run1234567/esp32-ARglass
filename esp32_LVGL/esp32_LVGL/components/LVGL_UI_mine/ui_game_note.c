/**
 * @file ui_game_note.c
 * @brief 声控跑酷小游戏 —— "音符君冲刺"
 *
 * 游戏玩法：
 *   - 沉默（<55dB）：静止不动
 *   - 轻哼（55~72dB）：慢速前进
 *   - 尖叫（≥72dB）：触发跳跃 + 地面加速
 *   - 碰到树木或掉进坑里：游戏结束
 *
 * 手势操作：
 *   - 未开始：右挥开始，左挥返回
 *   - 进行中：左挥强制退出
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include <stdlib.h>

// ============================================================
//   图片资源声明
// ============================================================
LV_IMG_DECLARE(tree);      // 树木障碍物
LV_IMG_DECLARE(people1);   // 玩家角色（音符君）

// ============================================================
//   全局/静态 UI 对象
// ============================================================
lv_obj_t * ui_game_note_screen;
static lv_obj_t * player;          // 玩家角色（图片）
static lv_obj_t * obstacle;        // 树木障碍物
static lv_obj_t * pit_obj;         // 坑洞遮罩
static lv_obj_t * floor_line;      // 地平线
static lv_obj_t * score_label;     // 计分板
static lv_obj_t * msg_label;       // 提示文字
static lv_obj_t * db_indicator;    // 顶部能量条
static lv_timer_t * note_timer = NULL;

// ============================================================
//   游戏参数
// ============================================================
#define SCREEN_W        240     // 屏幕宽度
#define SCREEN_H        240     // 屏幕高度
#define FLOOR_Y         180     // 地平线高度
#define PLAYER_SIZE     16      // 碰撞判定用的玩家尺寸
#define OBS_WIDTH       32      // 树木宽度
#define OBS_HEIGHT      26      // 树木高度
#define PIT_WIDTH       40      // 坑洞宽度
#define PLAYER_IMG_H    48      // 人物图片实际高度（用于贴地对齐）

// 声控阈值
#define DB_SILENCE      55      // 低于此 = 静止
#define DB_JUMP         72      // 高于此 = 跳跃

// ============================================================
//   游戏状态变量
// ============================================================
static int player_y = FLOOR_Y - PLAYER_IMG_H;
static int player_vel_y = 0;
static int obs_x = SCREEN_W;
static int pit_x = SCREEN_W + 150; // 坑洞初始放在屏幕外，与树木错开
static int game_score = 0;
static int current_db = 0;
static bool is_playing = false;
static bool is_grounded = true;

// ============================================================
//   分贝注入（串口回调调用，高频）
// ============================================================
void game_note_pass_db(int val) {
    current_db = val;
    // 尖叫 + 在地面 → 立刻起跳（零延迟响应）
    if (is_playing && val >= DB_JUMP && is_grounded) {
        player_vel_y = -12;
        is_grounded = false;
    }
}

// ============================================================
//   游戏结束
// ============================================================
static void game_over(void) {
    is_playing = false;
    if (note_timer) lv_timer_pause(note_timer);
    lv_label_set_text(msg_label, "Game Over!\n右挥重新开始\n左挥退出");
    lv_obj_clear_flag(msg_label, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================
//   碰撞检测（AABB 矩形碰撞 + 坑洞坠落）
// ============================================================
static bool check_collision(void) {
    // 玩家矩形边界（固定在 x=50）
    int p_left = 50, p_right = 50 + PLAYER_SIZE;
    int p_top = player_y, p_bottom = player_y + PLAYER_SIZE;

    // 碰树检测
    int o_left = obs_x, o_right = obs_x + OBS_WIDTH;
    int o_top = FLOOR_Y - OBS_HEIGHT, o_bottom = FLOOR_Y;
    if (p_right > o_left && p_left < o_right) {
        if (p_bottom > o_top && p_top < o_bottom) {
            return true;
        }
    }
    return false;
}

// ============================================================
//   主循环（约 30FPS，每 33ms 触发一次）
// ============================================================
static void note_game_loop(lv_timer_t * timer) {
    if (!is_playing) return;

    if (lvgl_port_lock(0)) {

        // ---- 1. 刷新顶部能量条 ----
        int bar_w = (current_db > 40) ? (current_db - 40) * 2 : 0;
        if (bar_w > SCREEN_W) bar_w = SCREEN_W;
        lv_obj_set_size(db_indicator, bar_w, 4);

        // ---- 2. 垂直物理（重力 + 坑洞坠落） ----
        int player_center_x = 50 + PLAYER_SIZE / 2;
        bool over_pit = (player_center_x > pit_x) && (player_center_x < pit_x + PIT_WIDTH);

        if (!is_grounded || over_pit) {
            player_vel_y += 1;     // 重力加速度
            player_y += player_vel_y;

            // 落地判定：脚底触地 且 不在坑上方
            if (player_y >= FLOOR_Y - PLAYER_IMG_H && !over_pit) {
                player_y = FLOOR_Y - PLAYER_IMG_H;
                player_vel_y = 0;
                is_grounded = true;
            }
            // 掉出屏幕 → 死亡
            else if (player_y > SCREEN_H) {
                game_over();
            }

            lv_obj_set_y(player, player_y);
        }

        // 刚走进坑里：从地面状态切换为滞空
        if (over_pit && is_grounded) {
            is_grounded = false;
        }

        // ---- 3. 水平滚动速度（声控） ----
        int scroll_speed = 0;
        if (current_db > DB_SILENCE) {
            scroll_speed = (current_db >= DB_JUMP) ? 6 : 3;
        }

        // ---- 4. 地面元素移动 ----
        if (scroll_speed > 0) {
            // 树木移动
            obs_x -= scroll_speed;
            if (obs_x < -OBS_WIDTH) {
                obs_x = SCREEN_W + (rand() % 40);
                game_score++;
                lv_label_set_text_fmt(score_label, "得分: %d", game_score);
            }
            lv_obj_set_x(obstacle, obs_x);

            // 坑洞移动
            pit_x -= scroll_speed;
            if (pit_x < -PIT_WIDTH) {
                pit_x = SCREEN_W + 100 + (rand() % 80); // 与树木错开
            }
            lv_obj_set_x(pit_obj, pit_x);
        }

        // ---- 5. 碰撞判定 ----
        if (check_collision()) {
            game_over();
        }

        lvgl_port_unlock();
    }
}

// ============================================================
//   初始化
// ============================================================
void ui_game_note_init(void) {
    ui_game_note_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_note_screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(ui_game_note_screen, 0, 0);

    // 顶部能量条
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

    // 玩家角色（替换为图片）
    player = lv_img_create(ui_game_note_screen);
    lv_img_set_src(player, &people1);
    lv_obj_set_pos(player, 50, player_y);
    lv_obj_set_style_bg_opa(player, 0, 0);
    lv_obj_set_style_border_width(player, 0, 0);

    // 坑洞（黑色遮罩，覆盖在地平线上模拟断路）
    pit_obj = lv_obj_create(ui_game_note_screen);
    lv_obj_set_size(pit_obj, PIT_WIDTH, 2);
    lv_obj_set_pos(pit_obj, pit_x, FLOOR_Y);
    lv_obj_set_style_bg_color(pit_obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(pit_obj, 0, 0);
    lv_obj_set_style_pad_all(pit_obj, 0, 0);

    // 树木障碍物（图片）
    obstacle = lv_img_create(ui_game_note_screen);
    lv_img_set_src(obstacle, &tree);
    lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT);
    lv_obj_set_style_bg_opa(obstacle, 0, 0);

    // 提示语
    msg_label = lv_label_create(ui_game_note_screen);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_label, &my_font_cn_16, 0);
    lv_label_set_text(msg_label, "右挥启动声控\n声音低: 前进\n尖叫: 跳跃");
    lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, -10);

    // 定时器（约 30FPS）
    note_timer = lv_timer_create(note_game_loop, 33, NULL);
    lv_timer_pause(note_timer);
}

// ============================================================
//   手势处理
// ============================================================
void game_note_screen_handle_cmd(ui_cmd_t cmd) {
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            // 重置所有状态
            player_y = FLOOR_Y - PLAYER_IMG_H;
            player_vel_y = 0;
            obs_x = SCREEN_W;
            pit_x = SCREEN_W + 150;
            game_score = 0;
            is_grounded = true;
            current_db = 0;

            lv_obj_add_flag(msg_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(score_label, "得分: 0");
            lv_obj_set_pos(player, 50, player_y);
            lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT);
            lv_obj_set_pos(pit_obj, pit_x, FLOOR_Y);

            is_playing = true;
            lv_timer_resume(note_timer);
        }
    } else {
        if (cmd == UI_CMD_LEFT) {
            is_playing = false;
            lv_timer_pause(note_timer);
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
    }
}
