/**
 * @file ui_game_2048_screen.c
 * @brief 2048游戏屏幕的完整实现文件
 *
 * 本文件实现了基于LVGL图形库的2048游戏，包含以下核心模块：
 *   1. 游戏界面的初始化与布局（极简白线框风格）
 *   2. 棋盘数据的绘制与刷新
 *   3. 随机生成新数字方块
 *   4. 滑动合并核心算法（上/下/左/右四个方向）
 *   5. 体感手势命令的处理与路由
 *
 * 游戏规则：
 *   - 在4x4的棋盘上，玩家通过四个方向的滑动操作移动所有数字方块
 *   - 相同数字的方块碰撞时会合并成一个，数值翻倍
 *   - 每次滑动后在空白位置随机生成一个2（90%概率）或4（10%概率）
 *   - 当棋盘填满且无法继续合并时，游戏结束
 *   - 目标是合成2048方块
 */

#include "ui_globals.h"      // 全局变量和通用定义（如字体、屏幕状态等）
#include "ui_manager.h"      // UI管理器，提供屏幕切换和命令类型定义
#include "esp_lvgl_port.h"   // ESP-IDF的LVGL端口适配层，提供lvgl_port_lock/unlock线程安全锁
#include <stdio.h>           // 标准输入输出库（用于格式化字符串等）
#include <stdlib.h>          // 标准库（用于rand()随机数生成）

// ==========================================
//   2048 游戏全局对象与核心矩阵
// ==========================================

/**
 * @brief 2048游戏的主屏幕对象指针
 *
 * 这是整个2048游戏界面的根容器，由ui_manager管理其生命周期。
 * 当切换到2048游戏界面时，此对象会被加载到显示器上。
 */
lv_obj_t  * ui_game_2048_screen;

/**
 * @brief 4x4网格中每个单元格的LVGL对象指针矩阵
 *
 * grid_cells[r][c] 指向第r行第c列的单元格容器对象。
 * 每个单元格是一个带黑色细线边框的白色小方块。
 * 用于存放和显示数字的背景容器。
 */
static lv_obj_t * grid_cells[4][4];

/**
 * @brief 4x4网格中每个单元格的数字标签对象指针矩阵
 *
 * grid_labels[r][c] 指向第r行第c列单元格内部的数字标签。
 * 每个标签独立创建并固定在对应单元格中，用于显示数字（如2、4、8...）。
 * 使用独立指针数组存储，避免每次都通过lv_obj_get_child()查找，
 * 提升查找稳定性并防止空指针崩溃。
 */
static lv_obj_t * grid_labels[4][4];

/**
 * @brief 分数显示标签对象指针
 *
 * 位于屏幕顶部，用于实时显示玩家当前的游戏分数。
 * 格式为"当前分数: XXX"。
 */
static lv_obj_t  * label_score;

/**
 * @brief 提示信息标签对象指针
 *
 * 位于屏幕中央，用于显示操作提示信息。
 * 游戏未开始时显示"右挥魔杖启动战局"，游戏开始后隐藏。
 */
static lv_obj_t  * label_msg;

/**
 * @brief 游戏核心数据矩阵（4x4棋盘）
 *
 * board[r][c] 存储第r行第c列的数值：
 *   - 值为0表示该位置为空（无方块）
 *   - 值为2、4、8、16、32、64、128、256、512、1024、2048等表示对应方块
 * 这是游戏逻辑的核心数据结构，所有的滑动、合并、生成操作都基于此矩阵。
 * 初始化为全0（空白棋盘）。
 */
static int board[4][4] = {0};

/**
 * @brief 当前游戏分数
 *
 * 每次合并方块时，合并后的数值会累加到分数中。
 * 例如：两个16合并成32，分数增加32分。
 * 游戏重新开始时重置为0。
 */
static int score = 0;

/**
 * @brief 游戏是否正在进行的标志位
 *
 * false: 游戏未开始，显示操作提示，等待玩家右挥启动
 * true:  游戏已开始，接受滑动操作，隐藏提示信息
 */
static bool is_playing = false;

// ==========================================
//   界面布局常量定义
// ==========================================

/**
 * @brief 每个小格子的边长尺寸（单位：像素）
 *
 * 设置为38像素，在240像素宽的屏幕上可以容纳4个格子加间隙。
 */
#define CELL_SIZE 38

/**
 * @brief 相邻格子之间的间隙宽度（单位：像素）
 *
 * 设置为6像素，提供清晰的视觉分隔。
 */
#define CELL_GAP  6

/**
 * @brief 整个棋盘底座的总尺寸（单位：像素）
 *
 * 计算公式：4个格子宽度 + 5个间隙（四周各1个 + 格子之间3个）
 * = 38 * 4 + 6 * 5 = 152 + 30 = 182 像素
 * 用于确定棋盘背景容器的大小。
 */
#define GRID_SIZE (CELL_SIZE * 4 + CELL_GAP * 5) // 动态计算：182

// ==========================================
//   视觉核心：纯白底板下的高饱和彩虹数字颜色分配
// ==========================================

/**
 * @brief 根据方块数值返回对应的高饱和度文字颜色
 *
 * 实现了彩虹色系的颜色分配方案：
 *   - 2:    黑色（0x000000）    - 最基础的数字，使用稳重的黑色
 *   - 4:    红色（0xFF0000）    - 鲜艳醒目
 *   - 8:    绿色（0x00FF00）    - 清新自然
 *   - 16:   蓝色（0x0000FF）    - 冷静深邃
 *   - 32:   深紫色（0x800080）  - 神秘高贵
 *   - 64:   天蓝色（0x87CEEB）  - 明亮通透
 *   - 128:  橙色（0xFF8C00）    - 温暖活力
 *   - 256:  青色（0x00FFFF）    - 科技感强
 *   - 512:  暗红（0x800000）    - 深沉有力
 *   - 1024: 深青（0x008080）    - 内敛含蓄
 *   - 2048: 霓虹紫（0xFF00FF） - 胜利的终极色彩
 *   - 其他: 灰色（0x808080）    - 未知或默认值的兜底颜色
 *
 * @param value 方块的数值（2的幂次方）
 * @return lv_color_t 对应的颜色值
 */
static lv_color_t get_tile_text_color(int value) {
    switch (value) {
        case 2:    return lv_color_hex(0x000000); // 黑色 - 最基础的数字
        case 4:    return lv_color_hex(0xFF0000); // 红色
        case 8:    return lv_color_hex(0x00FF00); // 绿色
        case 16:   return lv_color_hex(0x0000FF); // 蓝色
        case 32:   return lv_color_hex(0x800080); // 深紫色
        case 64:   return lv_color_hex(0x87CEEB); // 天蓝色
        case 128:  return lv_color_hex(0xFF8C00); // 橙色
        case 256:  return lv_color_hex(0x00FFFF); // 青色
        case 512:  return lv_color_hex(0x800000); // 暗红
        case 1024: return lv_color_hex(0x008080); // 深青
        case 2048: return lv_color_hex(0xFF00FF); // 霓虹紫 - 达成目标的胜利色
        default:   return lv_color_hex(0x808080); // 灰色 (用于未知或默认值)
    }
}

/**
 * @brief 刷新整个棋盘的视觉显示
 *
 * 此函数遍历4x4矩阵中的每一个单元格，根据board[][]中的数据
 * 更新对应的LVGL标签文字和颜色：
 *
 * 1. 将所有单元格背景设置为白色（极简风格）
 * 2. 如果单元格数值大于0：
 *    - 将标签文字设置为对应的数值
 *    - 将文字颜色设置为该数值对应的彩虹色（通过get_tile_text_color获取）
 * 3. 如果单元格数值为0（空白）：
 *    - 将标签文字清空，保持纯白干净
 * 4. 最后更新顶部的分数显示
 *
 * 此函数在以下场景被调用：
 *   - 游戏开始时初始化棋盘显示
 *   - 每次滑动合并后刷新棋盘状态
 */
static void draw_board(void) {
    /* 双重循环遍历4x4矩阵的每一行(r)和每一列(c) */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            /* 读取当前单元格的数值 */
            int val = board[r][c];

            /* 每一个单元格始终保持简单的白色背景（极简风格不变） */
            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);

            /* 根据数值是否大于0，决定显示内容 */
            if (val > 0) {
                /* 有数值：格式化显示数字，并设置对应的彩虹颜色 */
                lv_label_set_text_fmt(grid_labels[r][c], "%d", val);
                lv_obj_set_style_text_color(grid_labels[r][c], get_tile_text_color(val), 0);
            } else {
                /* 空白格子：清空文字，保持纯白干净的视觉效果 */
                lv_label_set_text(grid_labels[r][c], "");
            }
        }
    }
    /* 更新屏幕顶部的分数显示，格式为"当前分数: XXX" */
    lv_label_set_text_fmt(label_score, "当前分数: %d", score);
}

/**
 * @brief 在随机一个空白格子上生成一个新的数字方块
 *
 * 生成逻辑：
 * 1. 遍历整个4x4棋盘，收集所有值为0（空白）的位置坐标
 * 2. 如果存在空白位置：
 *    a. 从所有空白位置中随机选择一个
 *    b. 在该位置放置数字：90%概率生成2，10%概率生成4
 *       （通过 rand() % 10 == 0 判断，0~9共10个数，只有0满足条件）
 * 3. 如果没有空白位置（棋盘已满），则不执行任何操作
 *
 * 此函数在以下场景被调用：
 *   - 游戏开始时调用两次，生成初始的两个方块
 *   - 每次成功滑动合并后调用一次，补充新方块
 */
static void spawn_tile(void) {
    /**
     * empty_count: 记录当前棋盘上空白格子的数量
     * 最大值为16（全空棋盘），最小值为0（棋盘已满）
     */
    int empty_count = 0;

    /**
     * empty_cells[16][2]: 存储所有空白格子的坐标
     * 每个元素的第一维是行号(r)，第二维是列号(c)
     * 最多存储16个空白位置（4x4=16）
     */
    int empty_cells[16][2];

    /* 遍历整个棋盘，找出所有空白位置并记录坐标 */
    for(int r=0; r<4; r++) {
        for(int c=0; c<4; c++) {
            if(board[r][c] == 0) {
                /* 找到一个空白格子，记录其行列坐标 */
                empty_cells[empty_count][0] = r;  // 记录行号
                empty_cells[empty_count][1] = c;  // 记录列号
                empty_count++;  // 空白计数器加1
            }
        }
    }

    /* 只有存在空白位置时才生成新方块 */
    if(empty_count > 0) {
        /* 从所有空白位置中随机选择一个索引 */
        int idx = rand() % empty_count;

        /**
         * 在选中的空白位置生成数字：
         * - rand() % 10 == 0 时（概率10%）生成4
         * - 其他情况（概率90%）生成2
         * 这是2048游戏的标准概率分布
         */
        board[empty_cells[idx][0]][empty_cells[idx][1]] = (rand() % 10 == 0) ? 4 : 2;
    }
}

// ==========================================
//   2048 核心滑动合并算法
// ==========================================

/**
 * @brief 2048游戏的核心滑动与合并算法
 *
 * 根据玩家的操作方向，对4x4棋盘执行滑动和合并操作。
 * 算法遵循2048的标准规则：
 *   - 所有非零方块向指定方向滑动到最远位置
 *   - 相邻的相同数字方块合并为一个，数值翻倍
 *   - 每次滑动中，每个方块最多只能参与一次合并
 *   - 合并优先级：先滑动后合并，合并按滑动方向的先后顺序
 *
 * @param direction 滑动方向，取值为UI_CMD_LEFT/RIGHT/UP/DOWN
 * @return bool 如果有任何方块发生了移动或合并，返回true；否则返回false
 *
 * 算法详解（以左滑为例）：
 *   1. 逐行处理，每行从左到右遍历（跳过最左边的列，因为它无法再左移）
 *   2. 对于每个非零方块，计算它能滑动到的最远位置target_c
 *   3. 检查目标位置的左边是否有相同数值的方块，且该方块本轮未被合并过
 *   4. 如果可以合并：目标位置数值翻倍，原位置清空，累加分数
 *   5. 如果不能合并但可以移动：移动到目标位置，原位置清空
 *   6. last_merged变量防止同一方块在一次滑动中被合并多次
 *
 * 其他方向的逻辑类似，只是遍历顺序和移动方向不同：
 *   - 右滑：每行从右到左遍历，向右移动
 *   - 上滑：每列从上到下遍历，向上移动
 *   - 下滑：每列从下到上遍历，向下移动
 */
static bool slide_and_merge(ui_cmd_t direction) {
    /**
     * moved: 标记本轮滑动是否有任何方块发生了移动或合并
     * 如果没有任何变化，函数返回false，调用者不会生成新方块
     */
    bool moved = false;

    // ============================================================
    //   向左滑动合并（UI_CMD_LEFT）
    //   处理逻辑：每行从左到右扫描，方块向左移动和合并
    // ============================================================
    if (direction == UI_CMD_LEFT) {
        /* 外层循环：逐行处理（r = 行索引，0~3） */
        for (int r = 0; r < 4; r++) {
            /**
             * last_merged: 记录本行中最近一次合并发生的目标列位置
             * 初始化为-1表示本行尚未发生合并
             * 作用：防止同一个方块在一次滑动中被合并多次
             * 例如：[2,2,4] 左滑应该变成 [4,4,0] 而不是 [8,0,0]
             */
            int last_merged = -1;

            /* 内层循环：从第1列开始向右遍历（第0列已经是最左，无需处理） */
            for (int c = 1; c < 4; c++) {
                /* 跳过空白格子，空白格子不需要移动 */
                if (board[r][c] == 0) continue;

                /**
                 * target_c: 当前方块能滑动到的最远列位置
                 * 从当前位置向左寻找，直到遇到非零格子或到达边界
                 */
                int target_c = c;

                /* 向左滑动：找到最远的空白位置 */
                while (target_c > 0 && board[r][target_c - 1] == 0) {
                    target_c--;
                }

                /**
                 * 判断是否可以合并：
                 * 条件1: target_c > 0 —— 目标位置左边还有空间
                 * 条件2: board[r][target_c - 1] == board[r][c] —— 左边方块数值相同
                 * 条件3: last_merged != target_c - 1 —— 左边方块本轮未被合并过
                 * 三个条件同时满足才执行合并
                 */
                if (target_c > 0 && board[r][target_c - 1] == board[r][c] && last_merged != target_c - 1) {
                    /* 合并操作：目标位置数值翻倍 */
                    board[r][target_c - 1] *= 2;
                    /* 分数累加：合并后的数值加入总分 */
                    score += board[r][target_c - 1];
                    /* 原位置清空 */
                    board[r][c] = 0;
                    /* 记录本次合并的目标位置，防止重复合并 */
                    last_merged = target_c - 1;
                    /* 标记发生了变化 */
                    moved = true;
                }
                /**
                 * 如果不能合并，但方块可以移动（当前位置和目标位置不同）
                 * 则执行纯移动操作
                 */
                else if (target_c != c) {
                    /* 将方块移动到目标位置 */
                    board[r][target_c] = board[r][c];
                    /* 原位置清空 */
                    board[r][c] = 0;
                    /* 标记发生了变化 */
                    moved = true;
                }
                /* 如果既不能合并也不能移动，则不做任何操作 */
            }
        }
    }

    // ============================================================
    //   向右滑动合并（UI_CMD_RIGHT）
    //   处理逻辑：每行从右到左扫描，方块向右移动和合并
    // ============================================================
    else if (direction == UI_CMD_RIGHT) {
        /* 外层循环：逐行处理 */
        for (int r = 0; r < 4; r++) {
            /**
             * last_merged: 初始化为4（超出列索引范围0~3）
             * 表示本行右侧尚未发生合并
             */
            int last_merged = 4;

            /* 内层循环：从第2列开始向左遍历（第3列已经是最右，无需处理） */
            for (int c = 2; c >= 0; c--) {
                /* 跳过空白格子 */
                if (board[r][c] == 0) continue;

                /**
                 * target_c: 当前方块能滑动到的最远列位置
                 * 从当前位置向右寻找空白位置
                 */
                int target_c = c;

                /* 向右滑动：找到最远的空白位置 */
                while (target_c < 3 && board[r][target_c + 1] == 0) {
                    target_c++;
                }

                /**
                 * 判断是否可以合并（向右方向）：
                 * 条件1: target_c < 3 —— 目标位置右边还有空间
                 * 条件2: board[r][target_c + 1] == board[r][c] —— 右边方块数值相同
                 * 条件3: last_merged != target_c + 1 —— 右边方块本轮未被合并过
                 */
                if (target_c < 3 && board[r][target_c + 1] == board[r][c] && last_merged != target_c + 1) {
                    /* 合并操作 */
                    board[r][target_c + 1] *= 2;
                    score += board[r][target_c + 1];
                    board[r][c] = 0;
                    last_merged = target_c + 1;
                    moved = true;
                }
                /* 纯移动操作（位置发生变化时） */
                else if (target_c != c) {
                    board[r][target_c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }

    // ============================================================
    //   向上滑动合并（UI_CMD_UP）
    //   处理逻辑：每列从上到下扫描，方块向上移动和合并
    // ============================================================
    else if (direction == UI_CMD_UP) {
        /* 外层循环：逐列处理（c = 列索引，0~3） */
        for (int c = 0; c < 4; c++) {
            /**
             * last_merged: 初始化为-1，表示本列上方尚未发生合并
             */
            int last_merged = -1;

            /* 内层循环：从第1行开始向下遍历（第0行已经是最上，无需处理） */
            for (int r = 1; r < 4; r++) {
                /* 跳过空白格子 */
                if (board[r][c] == 0) continue;

                /**
                 * target_r: 当前方块能滑动到的最远行位置
                 * 从当前位置向上寻找空白位置
                 */
                int target_r = r;

                /* 向上滑动：找到最远的空白位置 */
                while (target_r > 0 && board[target_r - 1][c] == 0) {
                    target_r--;
                }

                /**
                 * 判断是否可以合并（向上方向）：
                 * 条件1: target_r > 0 —— 目标位置上方还有空间
                 * 条件2: board[target_r - 1][c] == board[r][c] —— 上方方块数值相同
                 * 条件3: last_merged != target_r - 1 —— 上方方块本轮未被合并过
                 */
                if (target_r > 0 && board[target_r - 1][c] == board[r][c] && last_merged != target_r - 1) {
                    /* 合并操作 */
                    board[target_r - 1][c] *= 2;
                    score += board[target_r - 1][c];
                    board[r][c] = 0;
                    last_merged = target_r - 1;
                    moved = true;
                }
                /* 纯移动操作 */
                else if (target_r != r) {
                    board[target_r][c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }

    // ============================================================
    //   向下滑动合并（UI_CMD_DOWN）
    //   处理逻辑：每列从下到上扫描，方块向下移动和合并
    // ============================================================
    else if (direction == UI_CMD_DOWN) {
        /* 外层循环：逐列处理 */
        for (int c = 0; c < 4; c++) {
            /**
             * last_merged: 初始化为4（超出行索引范围0~3）
             * 表示本列下方尚未发生合并
             */
            int last_merged = 4;

            /* 内层循环：从第2行开始向上遍历（第3行已经是最下，无需处理） */
            for (int r = 2; r >= 0; r--) {
                /* 跳过空白格子 */
                if (board[r][c] == 0) continue;

                /**
                 * target_r: 当前方块能滑动到的最远行位置
                 * 从当前位置向下寻找空白位置
                 */
                int target_r = r;

                /* 向下滑动：找到最远的空白位置 */
                while (target_r < 3 && board[target_r + 1][c] == 0) {
                    target_r++;
                }

                /**
                 * 判断是否可以合并（向下方向）：
                 * 条件1: target_r < 3 —— 目标位置下方还有空间
                 * 条件2: board[target_r + 1][c] == board[r][c] —— 下方方块数值相同
                 * 条件3: last_merged != target_r + 1 —— 下方方块本轮未被合并过
                 */
                if (target_r < 3 && board[target_r + 1][c] == board[r][c] && last_merged != target_r + 1) {
                    /* 合并操作 */
                    board[target_r + 1][c] *= 2;
                    score += board[target_r + 1][c];
                    board[r][c] = 0;
                    last_merged = target_r + 1;
                    moved = true;
                }
                /* 纯移动操作 */
                else if (target_r != r) {
                    board[target_r][c] = board[r][c];
                    board[r][c] = 0;
                    moved = true;
                }
            }
        }
    }

    /* 返回本轮是否有任何方块发生了移动或合并 */
    return moved;
}

// ==========================================
//   界面初始化（极简无瑕白线框风格）
// ==========================================

/**
 * @brief 初始化2048游戏的整个界面
 *
 * 此函数创建并配置游戏界面的所有视觉元素，采用极简白线框风格：
 *
 * 1. 创建游戏主屏幕对象，设置纯白色背景
 * 2. 在顶部创建分数显示标签（黑色文字）
 * 3. 创建棋盘底座容器（白色背景 + 黑色外边框）
 * 4. 在底座内创建4x4的单元格网格（白色背景 + 黑色细线边框）
 * 5. 在每个单元格内创建数字标签（用于显示方块数值）
 * 6. 在屏幕中央创建操作提示标签（深灰色文字）
 *
 * 布局计算（240像素宽屏幕）：
 *   - 棋盘总宽度 = 182像素（GRID_SIZE）
 *   - 棋盘水平居中位置 = (240 - 182) / 2 = 29像素
 *   - 每个单元格位置 = CELL_GAP + 索引 * (CELL_SIZE + CELL_GAP)
 *
 * 安全规范：
 *   - 初始化时调用不draw_board()，避免在LVGL对象尚未完全就绪时访问导致空指针崩溃
 *   - draw_board()只在游戏正式开始时（玩家右挥后）才被调用
 */
void ui_game_2048_init(void) {
    /* 创建游戏主屏幕对象（无父对象，作为独立屏幕） */
    ui_game_2048_screen = lv_obj_create(NULL);

    /* 设置整体游戏背景为极简纯白色 */
    lv_obj_set_style_bg_color(ui_game_2048_screen, lv_color_white(), 0);

    // ===== 1. 顶部得分板 =====

    /* 创建分数显示标签 */
    label_score = lv_label_create(ui_game_2048_screen);

    /* 设置文字颜色为黑色（在白色背景上清晰可见） */
    lv_obj_set_style_text_color(label_score, lv_color_black(), 0);

    /* 设置字体为自定义中文字体（16像素大小） */
    lv_obj_set_style_text_font(label_score, &my_font_cn_16, 0);

    /* 设置初始分数显示文本 */
    lv_label_set_text(label_score, "当前分数: 0");

    /* 手动定位分数标签：X=80（水平居中偏移），Y=5（顶部留白） */
    lv_obj_set_pos(label_score, 80, 5);

    // ===== 2. 创建棋盘底座容器 =====

    /* 创建棋盘背景容器，作为所有单元格的父对象 */
    lv_obj_t * board_bg = lv_obj_create(ui_game_2048_screen);

    /* 设置底座尺寸为182x182像素（GRID_SIZE） */
    lv_obj_set_size(board_bg, GRID_SIZE, GRID_SIZE);

    /**
     * 手动定位底座：
     * X = (240 - 182) / 2 = 29（水平居中）
     * Y = 40（在分数标签下方留出足够空间）
     */
    lv_obj_set_pos(board_bg, 29, 40);

    /* 底座背景设置为白色 */
    lv_obj_set_style_bg_color(board_bg, lv_color_white(), 0);

    /* 底座外边框设置为黑色，宽度2像素，形成明显的框架线 */
    lv_obj_set_style_border_color(board_bg, lv_color_black(), 0);
    lv_obj_set_style_border_width(board_bg, 2, 0);

    /* 底座圆角设置为4像素，使边框略带圆角 */
    lv_obj_set_style_radius(board_bg, 4, 0);

    /* 禁止棋盘背景容器的滚动功能（游戏界面不需要滚动） */
    lv_obj_clear_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* 清除底座的默认内边距，让单元格可以精确对齐到边缘 */
    lv_obj_set_style_pad_all(board_bg, 0, 0);

    // ===== 3. 创建4x4单元格网格 =====

    /**
     * 双重循环创建16个单元格
     * 每个单元格包含：
     *   - 一个带黑色细线边框的白色背景容器（grid_cells[r][c]）
     *   - 一个居中显示的数字标签（grid_labels[r][c]）
     */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            /* 创建单元格容器（父对象为棋盘底座） */
            grid_cells[r][c] = lv_obj_create(board_bg);

            /* 设置单元格尺寸为38x38像素 */
            lv_obj_set_size(grid_cells[r][c], CELL_SIZE, CELL_SIZE);

            /**
             * 计算单元格在底座内的位置：
             * x_pos = CELL_GAP + c * (CELL_SIZE + CELL_GAP)
             *       = 6 + c * 44
             * y_pos = CELL_GAP + r * (CELL_SIZE + CELL_GAP)
             *       = 6 + r * 44
             * 这样每个单元格之间都有6像素的间隙
             */
            int x_pos = CELL_GAP + c * (CELL_SIZE + CELL_GAP);
            int y_pos = CELL_GAP + r * (CELL_SIZE + CELL_GAP);
            lv_obj_set_pos(grid_cells[r][c], x_pos, y_pos);

            /* 单元格背景设置为白色（极简风格） */
            lv_obj_set_style_bg_color(grid_cells[r][c], lv_color_white(), 0);

            /* 单元格边框设置为黑色，宽度1像素，形成细线网格 */
            lv_obj_set_style_border_color(grid_cells[r][c], lv_color_black(), 0);
            lv_obj_set_style_border_width(grid_cells[r][c], 1, 0);

            /* 单元格圆角设置为2像素，与底座风格统一 */
            lv_obj_set_style_radius(grid_cells[r][c], 2, 0);

            /* 禁止单元格的滚动功能 */
            lv_obj_clear_flag(grid_cells[r][c], LV_OBJ_FLAG_SCROLLABLE);

            /* 清除单元格的默认内边距，让标签可以精确居中 */
            lv_obj_set_style_pad_all(grid_cells[r][c], 0, 0);

            /**
             * 在单元格内部创建数字标签
             * 此标签用于显示方块数值（如2、4、8、16...）
             * 初始状态为空（无文字），游戏开始后由draw_board()填充
             */
            grid_labels[r][c] = lv_label_create(grid_cells[r][c]);

            /* 设置标签字体为自定义中文字体 */
            lv_obj_set_style_text_font(grid_labels[r][c], &my_font_cn_16, 0);

            /* 将标签居中对齐在单元格内 */
            lv_obj_align(grid_labels[r][c], LV_ALIGN_CENTER, 0, 0);
        }
    }

    // ===== 4. 操作提示标签 =====

    /* 创建提示信息标签 */
    label_msg = lv_label_create(ui_game_2048_screen);

    /* 设置提示文字颜色为深灰色（0x555555），不喧宾夺主 */
    lv_obj_set_style_text_color(label_msg, lv_color_hex(0x555555), 0);

    /* 设置文字居中对齐 */
    lv_obj_set_style_text_align(label_msg, LV_TEXT_ALIGN_CENTER, 0);

    /* 设置字体为自定义中文字体 */
    lv_obj_set_style_text_font(label_msg, &my_font_cn_16, 0);

    /* 设置初始提示文本 */
    lv_label_set_text(label_msg, "👉 右挥魔杖启动战局");

    /**
     * 将提示标签居中对齐在屏幕上
     * LV_ALIGN_CENTER: 居中对齐
     * x_offset=0: 水平无偏移
     * y_offset=10: 垂直向下偏移10像素（稍微偏下，避免与棋盘重叠）
     */
    lv_obj_align(label_msg, LV_ALIGN_CENTER, 0, 10);

    /**
     * 安全规范说明：
     * 开机初始化时调用不draw_board()，全面防范空指针崩溃！
     * draw_board()只在游戏正式开始时（玩家右挥触发game_2048_screen_handle_cmd后）才调用。
     * 这是因为LVGL对象在init阶段可能尚未完全就绪，提前访问可能导致崩溃。
     */
}

// ==========================================
//   2048 专属体感手势控制路由
// ==========================================

/**
 * @brief 处理2048游戏界面的体感手势命令
 *
 * 此函数是2048游戏的命令处理入口，由UI管理器在检测到体感手势时调用。
 * 根据游戏当前状态（是否正在游玩）采用不同的处理逻辑：
 *
 * 【游戏未开始状态】(is_playing == false):
 *   - UI_CMD_LEFT（左挥）: 退出游戏，返回游戏列表界面
 *   - UI_CMD_RIGHT（右挥）: 开始新游戏
 *     1. 重置分数为0
 *     2. 清空棋盘（所有位置设为0）
 *     3. 隐藏操作提示标签
 *     4. 生成两个初始方块
 *     5. 绘制棋盘
 *     6. 设置is_playing为true
 *
 * 【游戏进行中状态】(is_playing == true):
 *   - 任何方向命令: 执行滑动合并操作
 *     1. 获取LVGL线程锁（保证线程安全）
 *     2. 调用slide_and_merge()执行滑动合并
 *     3. 如果有任何方块移动/合并：
 *        a. 生成一个新方块
 *        b. 重新绘制整个棋盘
 *     4. 释放LVGL线程锁
 *
 * @param cmd 体感手势命令，取值为UI_CMD_LEFT/RIGHT/UP/DOWN
 */
void game_2048_screen_handle_cmd(ui_cmd_t cmd) {
    /* 判断游戏是否未开始 */
    if (!is_playing) {
        /* === 游戏未开始状态 === */

        if (cmd == UI_CMD_LEFT) {
            /**
             * 左挥：退出游戏，返回游戏列表界面
             * 声明外部函数switch_to_screen()，用于切换屏幕
             */
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            /**
             * 右挥：开始新游戏
             * 执行完整的初始化流程
             */

            /* 重置分数为0 */
            score = 0;

            /* 清空整个棋盘（双重循环将所有位置设为0） */
            for(int r=0; r<4; r++) for(int c=0; c<4; c++) board[r][c] = 0;

            /**
             * 获取LVGL线程锁
             * lvgl_port_lock(0)中的参数0表示无限等待，直到获取到锁
             * 这是ESP-IDF LVGL端口提供的线程安全机制
             * 在多线程环境下，所有LVGL操作都必须在持锁状态下执行
             */
            if (lvgl_port_lock(0)) {
                /* 隐藏操作提示标签（游戏已开始，不再需要提示） */
                lv_obj_add_flag(label_msg, LV_OBJ_FLAG_HIDDEN);

                /* 生成两个初始方块（2048游戏标准开局） */
                spawn_tile();
                spawn_tile();

                /* 绘制棋盘（首次显示方块） */
                draw_board();

                /* 释放LVGL线程锁 */
                lvgl_port_unlock();
            }

            /* 设置游戏状态为进行中 */
            is_playing = true;
        }
    }
    else {
        /* === 游戏进行中状态 === */

        /**
         * 获取LVGL线程锁
         * 在游戏进行中，所有UI更新操作都需要在持锁状态下执行
         */
        if (lvgl_port_lock(0)) {
            /**
             * 执行滑动合并操作
             * slide_and_merge()会根据cmd方向处理棋盘数据
             * 返回true表示有任何方块发生了移动或合并
             */
            bool moved = slide_and_merge(cmd);

            /* 只有在棋盘状态发生变化时才生成新方块和刷新显示 */
            if (moved) {
                /**
                 * 生成一个新方块
                 * 每次成功滑动后补充一个新方块，保持游戏节奏
                 */
                spawn_tile();

                /**
                 * 重新绘制整个棋盘
                 * 刷新所有单元格的文字和颜色显示
                 */
                draw_board();
            }

            /* 释放LVGL线程锁 */
            lvgl_port_unlock();
        }
    }
}
