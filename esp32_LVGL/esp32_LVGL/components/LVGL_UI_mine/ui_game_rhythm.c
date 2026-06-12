/**
 * @file ui_game_rhythm.c
 * @brief 节奏魔杖 (极简版 Beat Saber)
 *
 * 四条垂直车道，音符从上往下掉落，挥动魔杖击碎。
 * 手势操作：
 * 【游玩中】：上/下/左/右挥动击碎对应车道音符，画圆暂停
 * 【暂停中】：上/下切换选项，右挥/画圆确认
 * 【未开始】：右挥开始，左挥退出
 */

#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdlib.h>

// ==========================================
//   游戏参数
// ==========================================
#define TRACK_W      160
#define TRACK_H      200
#define LANE_W       40
#define HIT_LINE_Y   160
#define NOTE_SIZE    30
#define MAX_NOTES    10
#define FALL_SPEED   4
#define MAX_PARTICLES 20
#define PARTICLE_SIZE 6

// ==========================================
//   全局对象与状态
// ==========================================
lv_obj_t * ui_game_rhythm_screen;
static lv_obj_t * track_bg;
static lv_obj_t * label_score;
static lv_obj_t * label_combo;
static lv_obj_t * label_feedback;

static lv_obj_t * note_objs[MAX_NOTES];
static lv_obj_t * note_labels[MAX_NOTES];
static bool note_active[MAX_NOTES] = {false};
static int  note_lane[MAX_NOTES]   = {0};
static int  note_y[MAX_NOTES]      = {0};

static lv_timer_t * game_timer = NULL;

// 粒子系统
typedef struct {
    lv_obj_t * obj;
    bool active;
    float x;
    float y;
    float vx;
    float vy;
    int life;
    int max_life;
} particle_t;

static particle_t particles[MAX_PARTICLES];

static int score = 0;
static int combo = 0;
static bool is_playing = false;
static int spawn_counter = 0;
static int feedback_timer = 0;

static bool is_paused = false;
static int pause_choice = 0;
static lv_obj_t * pause_overlay;
static lv_obj_t * label_pause_options[3];

// ==========================================
//   视觉辅助
// ==========================================
static lv_color_t get_lane_color(int lane) {
    switch (lane) {
        case 0: return lv_color_hex(0xFF0000); // 上：红
        case 1: return lv_color_hex(0x0088FF); // 下：蓝
        case 2: return lv_color_hex(0x00FF00); // 左：绿
        case 3: return lv_color_hex(0xFFCC00); // 右：黄
        default:return lv_color_white();
    }
}

static const char* get_lane_text(int lane) {
    switch (lane) {
        case 0: return "上";
        case 1: return "下";
        case 2: return "左";
        case 3: return "右";
        default:return "";
    }
}

static void update_pause_menu(void) {
    const char* texts[3] = {"继续游戏", "重新开始", "退出游戏"};
    for (int i = 0; i < 3; i++) {
        if (i == pause_choice) {
            lv_label_set_text_fmt(label_pause_options[i], "> %s <", texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0xFF0000), 0);
        } else {
            lv_label_set_text(label_pause_options[i], texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0x808080), 0);
        }
    }
}

static void show_feedback(const char* text, lv_color_t color) {
    lv_label_set_text(label_feedback, text);
    lv_obj_set_style_text_color(label_feedback, color, 0);
    lv_obj_clear_flag(label_feedback, LV_OBJ_FLAG_HIDDEN);
    feedback_timer = 15;
}

// ==========================================
//   核心逻辑
// ==========================================
static void spawn_note(void) {
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!note_active[i]) {
            int lane = rand() % 4;
            note_active[i] = true;
            note_lane[i] = lane;
            note_y[i] = -NOTE_SIZE;

            lv_obj_set_style_bg_color(note_objs[i], get_lane_color(lane), 0);
            lv_label_set_text(note_labels[i], get_lane_text(lane));
            lv_obj_set_pos(note_objs[i], lane * LANE_W + (LANE_W - NOTE_SIZE) / 2, note_y[i]);
            lv_obj_clear_flag(note_objs[i], LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
}

// 粒子爆炸效果
static void spawn_particles(int x, int y, lv_color_t color) {
    int to_spawn = 6;
    for (int i = 0; i < MAX_PARTICLES && to_spawn > 0; i++) {
        if (!particles[i].active) {
            particles[i].active = true;
            particles[i].x = x;
            particles[i].y = y;
            particles[i].vx = (rand() % 10 - 5) * 1.5f;
            particles[i].vy = -(rand() % 8 + 4);
            particles[i].max_life = 12 + (rand() % 6);
            particles[i].life = particles[i].max_life;

            lv_obj_set_style_bg_color(particles[i].obj, color, 0);
            lv_obj_set_style_opa(particles[i].obj, LV_OPA_COVER, 0);
            lv_obj_set_pos(particles[i].obj, (int)particles[i].x, (int)particles[i].y);
            lv_obj_clear_flag(particles[i].obj, LV_OBJ_FLAG_HIDDEN);

            to_spawn--;
        }
    }
}

static void process_hit(int target_lane) {
    int hit_index = -1;
    int max_y = -100;

    for (int i = 0; i < MAX_NOTES; i++) {
        if (note_active[i] && note_lane[i] == target_lane) {
            if (note_y[i] > max_y) {
                max_y = note_y[i];
                hit_index = i;
            }
        }
    }

    if (hit_index == -1 || max_y < HIT_LINE_Y - 40) {
        combo = 0;
        show_feedback("MISS", lv_color_hex(0x555555));
        lv_label_set_text_fmt(label_combo, "Combo: 0");
        return;
    }

    int distance = abs(max_y + NOTE_SIZE / 2 - HIT_LINE_Y);

    int hit_x = target_lane * LANE_W + (LANE_W - PARTICLE_SIZE) / 2;
    int hit_y = max_y + NOTE_SIZE / 2;
    lv_color_t lane_color = get_lane_color(target_lane);

    if (distance < 15) {
        score += 30; combo++;
        show_feedback("PERFECT!", lv_color_hex(0xFFD700));
        spawn_particles(hit_x, hit_y, lane_color);
    } else if (distance < 35) {
        score += 10; combo++;
        show_feedback("GOOD", lv_color_hex(0x00FF00));
        spawn_particles(hit_x, hit_y, lane_color);
    } else {
        combo = 0;
        show_feedback("MISS", lv_color_hex(0xFF0000));
    }

    note_active[hit_index] = false;
    lv_obj_add_flag(note_objs[hit_index], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(label_score, "Score: %d", score);
    lv_label_set_text_fmt(label_combo, "Combo: %d", combo);
}

static void game_tick_cb(lv_timer_t * timer) {
    if (!is_playing || is_paused) return;

    if (lvgl_port_lock(0)) {
        int spawn_rate = (combo > 20) ? 15 : 25;
        spawn_counter++;
        if (spawn_counter >= spawn_rate) {
            spawn_note();
            spawn_counter = 0;
        }

        for (int i = 0; i < MAX_NOTES; i++) {
            if (note_active[i]) {
                note_y[i] += FALL_SPEED + (combo / 15);
                lv_obj_set_y(note_objs[i], note_y[i]);

                if (note_y[i] > TRACK_H) {
                    note_active[i] = false;
                    lv_obj_add_flag(note_objs[i], LV_OBJ_FLAG_HIDDEN);
                    combo = 0;
                    show_feedback("MISS", lv_color_hex(0xFF0000));
                    lv_label_set_text(label_combo, "Combo: 0");
                }
            }
        }

        if (feedback_timer > 0) {
            feedback_timer--;
            if (feedback_timer == 0) {
                lv_obj_add_flag(label_feedback, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // 粒子物理更新
        for (int i = 0; i < MAX_PARTICLES; i++) {
            if (particles[i].active) {
                particles[i].x += particles[i].vx;
                particles[i].y += particles[i].vy;
                particles[i].vy += 1.2f; // 重力
                particles[i].life--;

                if (particles[i].life <= 0) {
                    particles[i].active = false;
                    lv_obj_add_flag(particles[i].obj, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_set_pos(particles[i].obj, (int)particles[i].x, (int)particles[i].y);
                    lv_opa_t opa = (particles[i].life * 255) / particles[i].max_life;
                    lv_obj_set_style_opa(particles[i].obj, opa, 0);
                }
            }
        }

        lvgl_port_unlock();
    }
}

void game_rhythm_pause_timer(void) {
    is_playing = false;
    is_paused = false;
    if (game_timer) lv_timer_pause(game_timer);
}

// ==========================================
//   初始化
// ==========================================
void ui_game_rhythm_init(void) {
    ui_game_rhythm_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_rhythm_screen, lv_color_black(), 0);

    // 顶部状态栏
    label_score = lv_label_create(ui_game_rhythm_screen);
    lv_obj_set_style_text_color(label_score, lv_color_white(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "Score: 0");
    lv_obj_align(label_score, LV_ALIGN_TOP_LEFT, 5, 5);

    label_combo = lv_label_create(ui_game_rhythm_screen);
    lv_obj_set_style_text_color(label_combo, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(label_combo, &my_font_cn_16, 0);
    lv_label_set_text(label_combo, "Combo: 0");
    lv_obj_align(label_combo, LV_ALIGN_TOP_RIGHT, -5, 5);

    // 轨道区域
    track_bg = lv_obj_create(ui_game_rhythm_screen);
    lv_obj_set_size(track_bg, TRACK_W, TRACK_H);
    lv_obj_align(track_bg, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(track_bg, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(track_bg, 1, 0);
    lv_obj_set_style_border_color(track_bg, lv_color_white(), 0);
    lv_obj_clear_flag(track_bg, LV_OBJ_FLAG_SCROLLABLE);

    // 车道分割线
    for (int i = 1; i < 4; i++) {
        lv_obj_t * line = lv_obj_create(track_bg);
        lv_obj_set_size(line, 1, TRACK_H);
        lv_obj_set_pos(line, i * LANE_W, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(line, 0, 0);
    }

    // 判定线
    lv_obj_t * hit_line = lv_obj_create(track_bg);
    lv_obj_set_size(hit_line, TRACK_W, 4);
    lv_obj_set_pos(hit_line, 0, HIT_LINE_Y);
    lv_obj_set_style_bg_color(hit_line, lv_color_white(), 0);
    lv_obj_set_style_border_width(hit_line, 0, 0);

    // 音符对象池
    for (int i = 0; i < MAX_NOTES; i++) {
        note_objs[i] = lv_obj_create(track_bg);
        lv_obj_set_size(note_objs[i], NOTE_SIZE, NOTE_SIZE);
        lv_obj_set_style_radius(note_objs[i], 4, 0);
        lv_obj_set_style_border_width(note_objs[i], 0, 0);
        lv_obj_clear_flag(note_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(note_objs[i], LV_OBJ_FLAG_HIDDEN);

        note_labels[i] = lv_label_create(note_objs[i]);
        lv_obj_set_style_text_font(note_labels[i], &my_font_cn_16, 0);
        lv_obj_set_style_text_color(note_labels[i], lv_color_black(), 0);
        lv_obj_align(note_labels[i], LV_ALIGN_CENTER, 0, 0);
    }

    // 粒子对象池
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particles[i].obj = lv_obj_create(track_bg);
        lv_obj_set_size(particles[i].obj, PARTICLE_SIZE, PARTICLE_SIZE);
        lv_obj_set_style_radius(particles[i].obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(particles[i].obj, 0, 0);
        lv_obj_clear_flag(particles[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(particles[i].obj, LV_OBJ_FLAG_HIDDEN);
        particles[i].active = false;
    }

    // 打击反馈
    label_feedback = lv_label_create(ui_game_rhythm_screen);
    lv_obj_set_style_text_font(label_feedback, &my_font_cn_16, 0);
    lv_obj_align(label_feedback, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(label_feedback, LV_OBJ_FLAG_HIDDEN);

    // 暂停菜单
    pause_overlay = lv_obj_create(ui_game_rhythm_screen);
    lv_obj_set_size(pause_overlay, 160, 160);
    lv_obj_align(pause_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pause_overlay, lv_color_white(), 0);
    lv_obj_set_style_border_color(pause_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(pause_overlay, 2, 0);
    lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        label_pause_options[i] = lv_label_create(pause_overlay);
        lv_obj_set_style_text_font(label_pause_options[i], &my_font_cn_16, 0);
        lv_obj_align(label_pause_options[i], LV_ALIGN_TOP_MID, 0, 30 + i * 40);
    }

    // 30FPS
    game_timer = lv_timer_create(game_tick_cb, 33, NULL);
    lv_timer_pause(game_timer);
}

// ==========================================
//   手势路由
// ==========================================
void game_rhythm_screen_handle_cmd(ui_cmd_t cmd) {
    // 未开始
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            score = 0; combo = 0; spawn_counter = 0;
            for (int i = 0; i < MAX_NOTES; i++) {
                note_active[i] = false;
            }
            if (lvgl_port_lock(0)) {
                for (int i = 0; i < MAX_NOTES; i++) {
                    lv_obj_add_flag(note_objs[i], LV_OBJ_FLAG_HIDDEN);
                }
                lv_label_set_text(label_score, "Score: 0");
                lv_label_set_text(label_combo, "Combo: 0");
                lv_obj_add_flag(label_feedback, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
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
                        score = 0; combo = 0; spawn_counter = 0;
                        lv_label_set_text(label_score, "Score: 0");
                        lv_label_set_text(label_combo, "Combo: 0");
                        for (int i = 0; i < MAX_NOTES; i++) {
                            note_active[i] = false;
                            lv_obj_add_flag(note_objs[i], LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_timer_resume(game_timer);
                    } else if (pause_choice == 2) {
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        is_playing = false;
                        lv_timer_pause(game_timer);
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
            if (lvgl_port_lock(0)) {
                if (cmd == UI_CMD_UP)         process_hit(0);
                else if (cmd == UI_CMD_DOWN)  process_hit(1);
                else if (cmd == UI_CMD_LEFT)  process_hit(2);
                else if (cmd == UI_CMD_RIGHT) process_hit(3);
                lvgl_port_unlock();
            }
        }
    }
}
