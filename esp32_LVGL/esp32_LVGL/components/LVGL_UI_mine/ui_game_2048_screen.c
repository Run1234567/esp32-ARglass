#include "ui_globals.h"
#include "ui_manager.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <stdlib.h>

// ==========================================
//   2048 游戏全局对象与核心矩阵
// ==========================================
lv_obj_t  * ui_game_2048_screen;
static lv_obj_t * grid_cells[4][4];  // 4x4的图形单元格对象
static lv_obj_t * grid_labels[4][4]; // 专有数字标签指针，稳固防崩溃
static lv_obj_t  * label_score;
static lv_obj_t  * label_msg;

// 游戏核心数据矩阵
static int board[4][4] = {0};
static int score = 0;
static bool is_playing = false;

#define CELL_SIZE 38    // 每个小格子的尺寸
#define CELL_GAP  6     // 格子之间的间隙
#define GRID_SIZE (CELL_SIZE * 4 + CELL_GAP * 5) // 动态计算：182

// ==========================================
//   ✨ 极简视觉核心：纯白底板下的高饱和彩虹数字分配
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
        default:   return lv_color_hex(0x808080); // 灰色 (用于未知或默认)
    }
}

// 刷新整个棋盘的显示（极简白色线框风）
static void draw_board(void) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int val = board[r][c];
            
            // 每一个单元格始终保持简单的白色背景
            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);
            
            // 刷新数字和对应分配的鲜艳颜色
            if (val > 0) {
                lv_label_set_text_fmt(grid_labels[r][c], "%d", val);
                lv_obj_set_style_text_color(grid_labels[r][c], get_tile_text_color(val), 0);
            } else {
                lv_label_set_text(grid_labels[r][c], ""); // 空格子保持纯白干净
            }
        }
    }
    lv_label_set_text_fmt(label_score, "当前分数: %d", score);
}

// 随机在一个空格子诞生一个数字 (2或4)
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
                } else if (target_c != c) {
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
                } else if (target_c != c) {
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
                } else if (target_r != r) {
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
                } else if (target_r != r) {
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
//   界面初始化（极简无瑕白线框风）
// ==========================================
void ui_game_2048_init(void) {
    ui_game_2048_screen = lv_obj_create(NULL);
    // 整体游戏大背景：极简纯白色
    lv_obj_set_style_bg_color(ui_game_2048_screen, lv_color_white(), 0);

    // 1. 顶部得分板 (黑色文字)
    label_score = lv_label_create(ui_game_2048_screen);
    lv_obj_set_style_text_color(label_score, lv_color_black(), 0);
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);
    lv_label_set_text(label_score, "当前分数: 0");
    lv_obj_set_pos(label_score, 80, 5); // 手动定位：X=80, Y=5

    // 2. 创建大棋盘底座（白色底色 + 黑色外边框）
    lv_obj_t * board_bg = lv_obj_create(ui_game_2048_screen);
    lv_obj_set_size(board_bg, GRID_SIZE, GRID_SIZE);
    lv_obj_set_pos(board_bg, 29, 40); // 手动定位：(240-182)/2=29, Y=40
    lv_obj_set_style_bg_color(board_bg, lv_color_white(), 0);
    lv_obj_set_style_border_color(board_bg, lv_color_black(), 0); // 黑色大框架线
    lv_obj_set_style_border_width(board_bg, 2, 0);
    lv_obj_set_style_radius(board_bg, 4, 0);
    lv_obj_clear_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE); // 禁止棋盘背景滚动
    lv_obj_set_style_pad_all(board_bg, 0, 0); // 清除大棋盘默认内边距

    // 3. 动态矩阵网格：绘制 16 个带有简单黑色细线框的白色单元格
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            grid_cells[r][c] = lv_obj_create(board_bg);
            lv_obj_set_size(grid_cells[r][c], CELL_SIZE, CELL_SIZE);
            
            int x_pos = CELL_GAP + c * (CELL_SIZE + CELL_GAP);
            int y_pos = CELL_GAP + r * (CELL_SIZE + CELL_GAP);
            lv_obj_set_pos(grid_cells[r][c], x_pos, y_pos);
            
            // 核心修改：简单的白色背景 + 简单的1像素黑色网格线框
            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);
            lv_obj_set_style_border_color(grid_cells[r][c], lv_color_black(), 0); 
            lv_obj_set_style_border_width(grid_cells[r][c], 1, 0);
            lv_obj_set_style_radius(grid_cells[r][c], 2, 0);
            lv_obj_clear_flag(grid_cells[r][c], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(grid_cells[r][c], 0, 0); // 清除小格子默认内边距

            // 内部嵌入高饱和度有色数字标签
            grid_labels[r][c] = lv_label_create(grid_cells[r][c]);
            lv_obj_set_style_text_font(grid_labels[r][c], &my_font_cn_16, 0);
            lv_obj_align(grid_labels[r][c], LV_ALIGN_CENTER, 0, 0);
        }
    }

    // 4. 全息操作提示语 (深灰色提示)
    label_msg = lv_label_create(ui_game_2048_screen);
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);
    lv_label_set_text(label_msg, "👉 右挥魔杖启动战局");
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 10);
    
    // 💡 遵循上一课安全规范：开机初始化时不调用 draw_board，全面防范空指针崩溃！
}

// ==========================================
//   2048 专属体感手势控制路由
// ==========================================
void game_2048_screen_handle_cmd(ui_cmd_t cmd) {
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
                spawn_tile();
                spawn_tile(); 
                draw_board(); // 只有开始玩的时候才正式绘制
                lvgl_port_unlock();
            }
            is_playing = true;
        }
    }
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