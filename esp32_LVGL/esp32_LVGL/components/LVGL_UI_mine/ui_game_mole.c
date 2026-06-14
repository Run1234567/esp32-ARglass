/**
 * @file ui_game_mole.c
 * @brief 体感打地鼠 —— 上下左右四向反应力挑战
 *
 * 手势操作：
 * 【游玩中】：对应方向挥动击打地鼠  画圆→暂停游戏
 * 【暂停中】：上挥→向上选选项  下挥→向下选选项  右挥/画圆→确认选择
 * 【未开始】：右挥→开始游戏  左挥→退出到游戏列表
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdlib.h>

// ==========================================
//   全局对象与状态
// ==========================================
LV_IMG_DECLARE(img_DIShu);

lv_obj_t * ui_game_mole_screen;
static lv_obj_t * label_score;
static lv_obj_t * label_msg;
static lv_obj_t * holes[4]; // 0:上, 1:下, 2:左, 3:右
static lv_obj_t * moles[4]; // 地鼠图片
static lv_timer_t * game_timer = NULL;

// 打击特效
static lv_obj_t * hit_labels[4];

static void anim_y_cb(void * var, int32_t v) {
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void anim_opa_cb(void * var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, v, 0);
}

static void anim_ready_cb(lv_anim_t * a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void show_hit_effect(int hole_idx) {
    lv_obj_t * label = hit_labels[hole_idx];
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(label, 255, 0);
    lv_obj_align_to(label, holes[hole_idx], LV_ALIGN_CENTER, 0, -10);
    lv_obj_update_layout(label);

    lv_coord_t start_y = lv_obj_get_y(label);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, label);
    lv_anim_set_values(&a_y, start_y, start_y - 30);
    lv_anim_set_time(&a_y, 400);
    lv_anim_set_exec_cb(&a_y, anim_y_cb);
    lv_anim_start(&a_y);

    lv_anim_t a_opa;
    lv_anim_init(&a_opa);
    lv_anim_set_var(&a_opa, label);
    lv_anim_set_values(&a_opa, 255, 0);
    lv_anim_set_time(&a_opa, 400);
    lv_anim_set_exec_cb(&a_opa, anim_opa_cb);
    lv_anim_set_ready_cb(&a_opa, anim_ready_cb);
    lv_anim_start(&a_opa);
}

static int score = 0;
static bool is_playing = false;
static int active_mole = -1; // 当前地鼠出现的位置 (0~3)，-1 表示没地鼠

// 暂停菜单
static bool is_paused = false;
static int pause_choice = 0;
static lv_obj_t * pause_overlay;
static lv_obj_t * label_pause_options[3];

// 刷新暂停菜单高亮
static void update_pause_menu(void) {
    const char* base_texts[3] = {"继续游戏", "重新开始", "退出游戏"};
    for (int i = 0; i < 3; i++) {
        if (i == pause_choice) {
            lv_label_set_text_fmt(label_pause_options[i], "> %s <", base_texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0xFF0000), 0);
        } else {
            lv_label_set_text(label_pause_options[i], base_texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0x808080), 0);
        }
    }
}

// 刷新四个地洞显示
static void draw_holes(void) {
    for (int i = 0; i < 4; i++) {
        if (i == active_mole) {
            lv_obj_clear_flag(moles[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(holes[i], lv_color_hex(0xFF0000), 0);
        } else {
            lv_obj_add_flag(moles[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_color(holes[i], lv_color_hex(0x555555), 0);
        }
    }
    lv_label_set_text_fmt(label_score, "Score: %d", score);
}

// 游戏心跳：随机生成地鼠
static void game_tick_cb(lv_timer_t * timer) {
    if (!is_playing || is_paused) return;

    if (lvgl_port_lock(0)) {
        if (rand() % 10 < 2) {
            active_mole = -1; // 20% 概率空洞
        } else {
            active_mole = rand() % 4;
        }
        draw_holes();
        lvgl_port_unlock();
    }
}

// 安全暂停（切出屏幕时调用）
void game_mole_pause_timer(void) {
    is_playing = false;
    is_paused = false;
    if (game_timer) lv_timer_pause(game_timer);
}

// ==========================================
//   界面初始化
// ==========================================
void ui_game_mole_init(void) {
    ui_game_mole_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_mole_screen, lv_color_black(), 0);

    // 得分板
    label_score = lv_label_create(ui_game_mole_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "Score: 0");
    lv_obj_align(label_score, LV_ALIGN_TOP_MID, 0, 5);

    // 4 个地洞与地鼠（十字分布）
    for (int i = 0; i < 4; i++) {
        holes[i] = lv_obj_create(ui_game_mole_screen);
        lv_obj_set_size(holes[i], 44, 44);
        lv_obj_set_style_radius(holes[i], 22, 0);
        lv_obj_set_style_border_width(holes[i], 2, 0);
        lv_obj_set_style_bg_color(holes[i], lv_color_hex(0x222222), 0);
        lv_obj_clear_flag(holes[i], LV_OBJ_FLAG_SCROLLABLE);

        moles[i] = lv_img_create(ui_game_mole_screen);
        lv_img_set_src(moles[i], &img_DIShu);
        lv_obj_add_flag(moles[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(holes[0], LV_ALIGN_CENTER, 0, -50);  // 上
    lv_obj_align(holes[1], LV_ALIGN_CENTER, 0,  50);   // 下
    lv_obj_align(holes[2], LV_ALIGN_CENTER, -60, 0);   // 左
    lv_obj_align(holes[3], LV_ALIGN_CENTER,  60, 0);   // 右

    for (int i = 0; i < 4; i++) {
        lv_obj_align_to(moles[i], holes[i], LV_ALIGN_CENTER, 0, 0);

        hit_labels[i] = lv_label_create(ui_game_mole_screen);
        lv_obj_set_style_text_color(hit_labels[i], lv_color_hex(0xFF3333), 0);
        lv_obj_set_style_text_font(hit_labels[i], &lv_font_montserrat_48, 0);
        lv_obj_set_style_shadow_color(hit_labels[i], lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_shadow_width(hit_labels[i], 12, 0);
        lv_obj_set_style_shadow_ofs_x(hit_labels[i], 0, 0);
        lv_obj_set_style_shadow_ofs_y(hit_labels[i], 0, 0);
        lv_label_set_text(hit_labels[i], "+10");
        lv_obj_add_flag(hit_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    draw_holes();

    // 提示标签
    label_msg = lv_label_create(ui_game_mole_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_label_set_text(label_msg, "右挥开始\n画圆暂停");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 0);

    // 暂停菜单覆盖层
    pause_overlay = lv_obj_create(ui_game_mole_screen);
    lv_obj_set_size(pause_overlay, 160, 160);
    lv_obj_align(pause_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pause_overlay, lv_color_white(), 0);
    lv_obj_set_style_border_color(pause_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(pause_overlay, 2, 0);
    lv_obj_set_style_radius(pause_overlay, 8, 0);
    lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        label_pause_options[i] = lv_label_create(pause_overlay);
        lv_obj_set_style_text_font(label_pause_options[i], &my_font_cn_16, 0);
        lv_obj_align(label_pause_options[i], LV_ALIGN_TOP_MID, 0, 20 + i * 40);
    }

    // 定时器（800ms 变一次地鼠）
    game_timer = lv_timer_create(game_tick_cb, 800, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   手势控制路由
// ==========================================
void game_mole_screen_handle_cmd(ui_cmd_t cmd) {
    // 1. 游戏未开始
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            score = 0;
            active_mole = -1;
            if (lvgl_port_lock(0)) {
                for (int i = 0; i < 4; i++) {
                    lv_anim_del(hit_labels[i], NULL);
                    lv_obj_add_flag(hit_labels[i], LV_OBJ_FLAG_HIDDEN);
                }
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                draw_holes();
                lvgl_port_unlock();
            }
            is_playing = true;
            is_paused = false;
            lv_timer_resume(game_timer);
        }
    }
    // 2. 暂停菜单交互
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
                case UI_CMD_CIRCLE:
                    if (pause_choice == 0) {
                        // 继续游戏
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        lv_timer_resume(game_timer);
                    }
                    else if (pause_choice == 1) {
                        // 重新开始
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        score = 0;
                        active_mole = -1;
                        for (int i = 0; i < 4; i++) {
                            lv_anim_del(hit_labels[i], NULL);
                            lv_obj_add_flag(hit_labels[i], LV_OBJ_FLAG_HIDDEN);
                        }
                        draw_holes();
                        lv_timer_resume(game_timer);
                    }
                    else if (pause_choice == 2) {
                        // 退出游戏
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_clear_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        is_playing = false;
                        lv_timer_pause(game_timer);
                        extern void switch_to_screen(ui_screen_state_t target);
                        switch_to_screen(SCREEN_GAME_LIST);
                    }
                    break;
                default:
                    break;
            }
            lvgl_port_unlock();
        }
    }
    // 3. 正常游玩
    else {
        if (cmd == UI_CMD_CIRCLE) {
            // 画圆 → 呼出暂停菜单
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
            // 判定击打
            int hit_target = -1;
            if (cmd == UI_CMD_UP)    hit_target = 0;
            if (cmd == UI_CMD_DOWN)  hit_target = 1;
            if (cmd == UI_CMD_LEFT)  hit_target = 2;
            if (cmd == UI_CMD_RIGHT) hit_target = 3;

            if (hit_target != -1 && hit_target == active_mole) {
                if (lvgl_port_lock(0)) {
                    score += 10;
                    show_hit_effect(hit_target);
                    active_mole = -1;
                    draw_holes();
                    lvgl_port_unlock();
                }
            }
        }
    }
}
