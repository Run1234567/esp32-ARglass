/**
 * @file ui_game_2048_screen.c
 * @brief 2048游戏屏幕的完整实现文件（含体感画圆暂停功能）
 */

#include "ui_globals.h"      // 全局变量和通用定义
#include "ui_manager.h"      // UI管理器，提供屏幕切换和命令类型定义
#include "esp_lvgl_port.h"   // ESP-IDF的LVGL端口适配层
#include <stdio.h> 
#include <stdlib.h> 

// ==========================================
//   2048 游戏全局对象与核心矩阵
// ==========================================

lv_obj_t  * ui_game_2048_screen;
static lv_obj_t * grid_cells[4][4];
static lv_obj_t * grid_labels[4][4];
static lv_obj_t  * label_score;
static lv_obj_t  * label_msg;

// ==== 【新增】暂停菜单相关对象与状态 ====
static bool is_paused = false;         // 暂停状态标志位
static int pause_choice = 0;           // 当前暂停选项索引 (0:继续, 1:重新开始, 2:退出)
static lv_obj_t * pause_overlay;       // 暂停菜单覆盖层容器
static lv_obj_t * label_pause_options[3]; // 3个菜单选项的标签指针

static int board[4][4] = {0};
static int score = 0;
static bool is_playing = false;

// ==========================================
//   界面布局常量定义
// ==========================================
#define CELL_SIZE 38
#define CELL_GAP   6
#define GRID_SIZE (CELL_SIZE * 4 + CELL_GAP * 5) // 182 像素

// ==========================================
//   视觉核心：高饱和彩虹数字颜色分配
// ==========================================
static lv_color_t get_tile_text_color(int value) {
    switch (value) {
        case 2:    return lv_color_hex(0x000000); // 黑色
        case 4:    return lv_color_hex(0xFF0000); // 红色
        case 8:    return lv_color_hex(0x00FF00); // 绿色
        case 16:   return lv_color_hex(0x0000FF); // 蓝色
        case 32:   return lv_color_hex(0x800080); // 深紫色
        case 64:   return lv_color_hex(0x87CEEB); // 天蓝色
        case 128:  return lv_color_hex(0xFF8C00); // 橙色
        case 256:  return lv_color_hex(0x00FFFF); // 青色
        case 512:  return lv_color_hex(0x800000); // 暗红
        case 1024: return lv_color_hex(0x008080); // 深青
        case 2048: return lv_color_hex(0xFF00FF); // 霓虹紫
        default:   return lv_color_hex(0x808080); // 灰色
    }
}

// 刷新整个棋盘的视觉显示
static void draw_board(void) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int val = board[r][c];
            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);
            if (val > 0) {
                lv_label_set_text_fmt(grid_labels[r][c], "%d", val);
                lv_obj_set_style_text_color(grid_labels[r][c], get_tile_text_color(val), 0);
            } else {
                lv_label_set_text(grid_labels[r][c], "");
            }
        }
    }
    lv_label_set_text_fmt(label_score, "当前分数: %d", score);
}

// 在随机一个空白格子上生成一个新的数字方块
static void spawn_tile(void) {
    int empty_count = 0;
    int empty_cells[16][2];

    for(int r=0; r<4; r++) {
        for(int c=0; c<4; c++) {
            if(board[r][c] == 0) {
                empty_cells[empty_count][0] = r;
                empty_cells[empty_count][1] = c;
                empty_count++;
            }
        }
    }

    if(empty_count > 0) {
        int idx = rand() % empty_count;
        board[empty_cells[idx][0]][empty_cells[idx][1]] = (rand() % 10 == 0) ? 4 : 2;
    }
}

// ==== 【新增】实时刷新暂停菜单的高亮视觉状态 ====
static void update_pause_menu(void) {
    const char* base_texts[3] = {"继续游戏", "重新开始", "退出游戏"};
    for (int i = 0; i < 3; i++) {
        if (i == pause_choice) {
            // 被选中的条目：加上视觉指示符，并变成高亮红色
            lv_label_set_text_fmt(label_pause_options[i], "> %s <", base_texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0xFF0000), 0);
        } else {
            // 未选中的条目：普通文本，保持低调灰色
            lv_label_set_text(label_pause_options[i], base_texts[i]);
            lv_obj_set_style_text_color(label_pause_options[i], lv_color_hex(0x808080), 0);
        }
    }
}

// ==========================================
//   2048 核心滑动合并算法
// ==========================================
static bool slide_and_merge(ui_cmd_t direction) {
    bool moved = false;

    // 向左滑动合并
    if (direction == UI_CMD_LEFT) {
        for (int r = 0; r < 4; r++) {
            int last_merged = -1;
            for (int c = 1; c < 4; c++) {
                if (board[r][c] == 0) continue;
                int target_c = c;
                while (target_c > 0 && board[r][target_c - 1] == 0) {
                    target_c--;
                }
                if (target_c > 0 && board[r][target_c - 1] == board[r][c] && last_merged != target_c - 1) {
                    board[r][target_c - 1] *= 2;
                    score += board[r][target_c - 1];
                    board[r][c] = 0;
                    last_merged = target_c - 1;
                    moved = true;
                }
                else if (target_c != c) {
                    board[r][target_c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }
    // 向右滑动合并
    else if (direction == UI_CMD_RIGHT) {
        for (int r = 0; r < 4; r++) {
            int last_merged = 4;
            for (int c = 2; c >= 0; c--) {
                if (board[r][c] == 0) continue;
                int target_c = c;
                while (target_c < 3 && board[r][target_c + 1] == 0) {
                    target_c++;
                }
                if (target_c < 3 && board[r][target_c + 1] == board[r][c] && last_merged != target_c + 1) {
                    board[r][target_c + 1] *= 2;
                    score += board[r][target_c + 1];
                    board[r][c] = 0;
                    last_merged = target_c + 1;
                    moved = true;
                }
                else if (target_c != c) {
                    board[r][target_c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }
    // 向上滑动合并
    else if (direction == UI_CMD_UP) {
        for (int c = 0; c < 4; c++) {
            int last_merged = -1;
            for (int r = 1; r < 4; r++) {
                if (board[r][c] == 0) continue;
                int target_r = r;
                while (target_r > 0 && board[target_r - 1][c] == 0) {
                    target_r--;
                }
                if (target_r > 0 && board[target_r - 1][c] == board[r][c] && last_merged != target_r - 1) {
                    board[target_r - 1][c] *= 2;
                    score += board[target_r - 1][c];
                    board[r][c] = 0;
                    last_merged = target_r - 1;
                    moved = true;
                }
                else if (target_r != r) {
                    board[target_r][c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }
    // 向下滑动合并
    else if (direction == UI_CMD_DOWN) {
        for (int c = 0; c < 4; c++) {
            int last_merged = 4;
            for (int r = 2; r >= 0; r--) {
                if (board[r][c] == 0) continue;
                int target_r = r;
                while (target_r < 3 && board[target_r + 1][c] == 0) {
                    target_r++;
                }
                if (target_r < 3 && board[target_r + 1][c] == board[r][c] && last_merged != target_r + 1) {
                    board[target_r + 1][c] *= 2;
                    score += board[target_r + 1][c];
                    board[r][c] = 0;
                    last_merged = target_r + 1;
                    moved = true;
                }
                else if (target_r != r) {
                    board[target_r][c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }

    return moved;
}

// ==========================================
//   界面初始化（极简无瑕白线框风格）
// ==========================================
void ui_game_2048_init(void) {
    ui_game_2048_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_2048_screen, lv_color_white(), 0);

    // ===== 1. 顶部得分板 =====
    label_score = lv_label_create(ui_game_2048_screen);
    lv_obj_set_style_text_color(label_score, lv_color_black(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "当前分数: 0");
    lv_obj_set_pos(label_score, 80, 5);

    // ===== 2. 创建棋盘底座容器 =====
    lv_obj_t * board_bg = lv_obj_create(ui_game_2048_screen);
    lv_obj_set_size(board_bg, GRID_SIZE, GRID_SIZE);
    lv_obj_set_pos(board_bg, 29, 40);
    lv_obj_set_style_bg_color(board_bg, lv_color_white(), 0);
    lv_obj_set_style_border_color(board_bg, lv_color_black(), 0);
    lv_obj_set_style_border_width(board_bg, 2, 0);
    lv_obj_set_style_radius(board_bg, 4, 0);
    lv_obj_clear_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(board_bg, 0, 0);

    // ===== 3. 创建4x4单元格网格 =====
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            grid_cells[r][c] = lv_obj_create(board_bg);
            lv_obj_set_size(grid_cells[r][c], CELL_SIZE, CELL_SIZE);

            int x_pos = CELL_GAP + c * (CELL_SIZE + CELL_GAP);
            int y_pos = CELL_GAP + r * (CELL_SIZE + CELL_GAP);
            lv_obj_set_pos(grid_cells[r][c], x_pos, y_pos);

            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);
            lv_obj_set_style_border_color(grid_cells[r][c], lv_color_black(), 0);
            lv_obj_set_style_border_width(grid_cells[r][c], 1, 0);
            lv_obj_set_style_radius(grid_cells[r][c], 2, 0);
            lv_obj_clear_flag(grid_cells[r][c], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(grid_cells[r][c], 0, 0);

            grid_labels[r][c] = lv_label_create(grid_cells[r][c]);
            lv_obj_set_style_text_font(grid_labels[r][c], &my_font_cn_16, 0);
            lv_obj_align(grid_labels[r][c], LV_ALIGN_CENTER, 0, 0);
        }
    }

    // ===== 4. 操作提示标签 =====
    label_msg = lv_label_create(ui_game_2048_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_label_set_text(label_msg, "👉 右挥魔杖启动战局");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 10);

    // ===== 5. 【核心新增】创建暂停菜单覆盖层 =====
    pause_overlay = lv_obj_create(ui_game_2048_screen);
    lv_obj_set_size(pause_overlay, GRID_SIZE, GRID_SIZE);
    lv_obj_set_pos(pause_overlay, 29, 40); // 精确盖在棋盘底座上
    lv_obj_set_style_bg_color(pause_overlay, lv_color_white(), 0);
    lv_obj_set_style_border_color(pause_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(pause_overlay, 2, 0);
    lv_obj_set_style_radius(pause_overlay, 4, 0);
    lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(pause_overlay, 0, 0);
    
    // 初始化默认隐藏
    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);

    // 预组装3个垂直均匀分布的文本标签
    for (int i = 0; i < 3; i++) {
        label_pause_options[i] = lv_label_create(pause_overlay);
        lv_obj_set_style_text_font(label_pause_options[i], &my_font_cn_16, 0);
        lv_obj_align(label_pause_options[i], LV_ALIGN_TOP_MID, 0, 32 + i * 42);
    }
}

// ==========================================
//   2048 专属体感手势控制路由
// ==========================================
void game_2048_screen_handle_cmd(ui_cmd_t cmd) {
    /* 1. 【状态分支：游戏未开始】 */
    if (!is_playing) {
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            score = 0;
            for(int r=0; r<4; r++) for(int c=0; c<4; c++) board[r][c] = 0;

            if (lvgl_port_lock(0)) {
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN); // 确保隐藏暂停框
                spawn_tile();
                spawn_tile();
                draw_board();
                lvgl_port_unlock();
            }
            is_playing = true;
            is_paused = false;
        }
    }
    /* 2. 【核心新增：暂停菜单交互状态】 */
    else if (is_paused) {
        if (lvgl_port_lock(0)) {
            switch (cmd) {
                case UI_CMD_UP:   // 菜单向上滚动
                    pause_choice = (pause_choice - 1 + 3) % 3;
                    update_pause_menu();
                    break;
                    
                case UI_CMD_DOWN: // 菜单向下滚动
                    pause_choice = (pause_choice + 1) % 3;
                    update_pause_menu();
                    break;
                    
                case UI_CMD_RIGHT: // 右挥确认当前选中的指令
                    if (pause_choice == 0) { 
                        // ---- 继续游戏 ----
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                    } 
                    else if (pause_choice == 1) { 
                        // ---- 重新开始 ----
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        is_paused = false;
                        score = 0;
                        for(int r=0; r<4; r++) for(int c=0; c<4; c++) board[r][c] = 0;
                        spawn_tile();
                        spawn_tile();
                        draw_board();
                    } 
                    else if (pause_choice == 2) { 
                        // ---- 退出游戏 ----
                        lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_clear_flag(label_msg, LV_OBJ_FLAG_HIDDEN); // 恢复初始引导语
                        is_paused = false;
                        is_playing = false;
                        extern void switch_to_screen(ui_screen_state_t target);
                        switch_to_screen(SCREEN_GAME_LIST);
                    }
                    break;
                    
                case UI_CMD_CIRCLE: // 再次画圆快速关闭暂停并恢复
                    lv_obj_add_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN);
                    is_paused = false;
                    break;
                    
                default:
                    break;
            }
            lvgl_port_unlock();
        }
    }
    /* 3. 【状态分支：游戏正常运行中】 */
    else {
        // 捕获画圆指令，拦截常规滑动，切入暂停模式
        if (cmd == UI_CMD_CIRCLE) {
            if (lvgl_port_lock(0)) {
                pause_choice = 0; // 重置光标，默认停在“继续游戏”
                update_pause_menu();
                lv_obj_clear_flag(pause_overlay, LV_OBJ_FLAG_HIDDEN); // 弹出暂停面板
                lvgl_port_unlock();
            }
            is_paused = true;
        }
        // 处理常规体感滑动：上下左右
        else {
            if (lvgl_port_lock(0)) {
                bool moved = slide_and_merge(cmd);
                if (moved) {
                    spawn_tile();
                    draw_board();
                }
                lvgl_port_unlock();
            }
        }
    }
}
