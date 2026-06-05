/**
 * @file ui_game_flappy.c
 * @brief Flappy Bird 风格小游戏的完整实现
 *
 * 本文件实现了一个适配 240x240 像素屏幕的 Flappy Bird 小游戏。
 * 玩家通过"上挥"手势控制小鸟跳跃，躲避上下两根管道组成的障碍物，
 * 穿过管道缝隙即可得分，撞到管道或屏幕上下边界则游戏结束。
 *
 * 游戏采用 LVGL 图形库进行 UI 绘制，使用 lv_timer 作为游戏主循环
 * （约 30FPS），通过手势命令（ui_cmd_t）驱动玩家输入。
 */

/* ===================== 头文件包含 ===================== */

#include "ui_globals.h"       // 全局 UI 变量与共享资源（如字体定义 my_font_cn_16 等）
#include "ui_manager.h"       // UI 管理器，提供 ui_cmd_t 命令枚举、屏幕状态枚举等
#include "esp_lvgl_port.h"    // ESP-IDF 的 LVGL 移植层，提供 lvgl_port_lock/unlock 线程安全接口
#include <stdlib.h>           // 标准库，提供 rand() 函数用于随机生成管道缺口位置

/* ===================== 图片资源声明 ===================== */

/**
 * LV_IMG_DECLARE 宏：声明一个外部定义的 LVGL 图片资源。
 * XSBird 是小鸟的图片数据，实际定义在其他 .c 文件或资源文件中，
 * 此处仅声明以便本文件引用。
 */
LV_IMG_DECLARE(XSBird);

/* ===================== 全局 UI 对象指针 ===================== */

/**
 * ui_game_flappy_screen - Flappy Bird 游戏的主屏幕对象（顶层容器）
 * 被 ui_manager 管理，用于在不同游戏/界面之间切换。
 */
lv_obj_t * ui_game_flappy_screen;

/**
 * bird - 小鸟的图片控件对象
 * 使用 lv_img 控件显示小鸟精灵图（XSBird），其 Y 坐标在游戏循环中不断更新。
 */
static lv_obj_t * bird;

/**
 * pipe_top - 上方管道的矩形控件对象
 * 用一个纯白色背景、黑色边框的 lv_obj 矩形模拟管道。
 * 高度会随管道缺口位置动态变化。
 */
static lv_obj_t * pipe_top;

/**
 * pipe_bottom - 下方管道的矩形控件对象
 * 与 pipe_top 配对，中间形成通道供小鸟穿越。
 */
static lv_obj_t * pipe_bottom;

/**
 * flappy_score_label - 得分显示标签
 * 显示当前得分，文本格式为 "得分: X"，位于屏幕顶部居中。
 */
static lv_obj_t * flappy_score_label;

/**
 * flappy_msg_label - 游戏提示/消息标签
 * 游戏开始前显示操作说明，游戏结束后显示 "Game Over" 及重新开始的提示。
 */
static lv_obj_t * flappy_msg_label;

/**
 * game_timer - 游戏主循环定时器
 * 由 lv_timer_create 创建，每 33ms 触发一次（约 30FPS），
 * 在回调函数 flappy_game_loop 中执行物理模拟和碰撞检测。
 * 初始为暂停状态，等玩家触发"右挥"手势后才启动。
 */
static lv_timer_t * game_timer = NULL;

/* ===================== 游戏物理参数宏定义 ===================== */

/**
 * GRAVITY - 重力加速度
 * 每帧小鸟的垂直速度（bird_velocity）会增加此值，模拟向下的重力效果。
 * 值越大，小鸟下落越快。
 */
#define GRAVITY         1

/**
 * JUMP_STRENGTH - 跳跃力度
 * 负值表示向上运动。当玩家触发跳跃时，bird_velocity 被设置为此值，
 * 随后每帧受 GRAVITY 影响逐渐减小（速度从负变正 = 先上升后下降）。
 */
#define JUMP_STRENGTH  -12

/**
 * PIPE_SPEED - 管道水平移动速度
 * 每帧管道向左移动的像素数。值越大，游戏难度越高。
 */
#define PIPE_SPEED      5

/**
 * PIPE_GAP - 上下管道之间的通道间距（像素）
 * 小鸟需要穿过这个间隙。值越大，游戏越简单。
 * 该值以 pipe_gap_y 为中心，上下各占一半。
 */
#define PIPE_GAP        85

/**
 * BIRD_WIDTH - 小鸟图片的宽度（像素）
 * 用于碰撞检测时计算小鸟的右边界。
 */
#define BIRD_WIDTH      30

/**
 * BIRD_HEIGHT - 小鸟图片的高度（像素）
 * 用于碰撞检测时计算小鸟的下边界，以及触顶/触底判断。
 */
#define BIRD_HEIGHT     19

/**
 * PIPE_WIDTH - 管道的宽度（像素）
 * 管道矩形控件的宽度，也是碰撞检测时判断小鸟是否进入管道 X 区间的依据。
 */
#define PIPE_WIDTH      35

/**
 * SCREEN_W - 屏幕宽度（像素）
 * 240x240 屏幕的宽度，用于管道重置位置和边界检测。
 */
#define SCREEN_W        240

/**
 * SCREEN_H - 屏幕高度（像素）
 * 240x240 屏幕的高度，用于触底检测和管道底端计算。
 */
#define SCREEN_H        240

/* ===================== 游戏运行时状态变量 ===================== */

/**
 * bird_y - 小鸟当前的 Y 坐标（像素）
 * 表示小鸟图片左上角在屏幕上的垂直位置。
 * 每帧根据 bird_velocity 更新：bird_y += bird_velocity。
 * 初始值 120 表示小鸟起始在屏幕垂直中央。
 */
static int bird_y = 120;

/**
 * bird_velocity - 小鸟当前的垂直速度（像素/帧）
 * 正值表示向下运动，负值表示向上运动。
 * 每帧受重力影响：bird_velocity += GRAVITY。
 * 跳跃时被设为 JUMP_STRENGTH（负值），之后逐渐增大变为正值（下落）。
 */
static int bird_velocity = 0;

/**
 * pipe_x - 管道组当前的 X 坐标（像素）
 * 上下管道共享同一个 X 坐标，表示管道左边缘的水平位置。
 * 每帧向左移动 PIPE_SPEED 像素。当移出屏幕左侧时重置到屏幕右侧。
 * 初始值 240 表示管道从屏幕右边缘开始。
 */
static int pipe_x = 240;

/**
 * pipe_gap_y - 管道通道中心点的 Y 坐标（像素）
 * 上下管道之间的通道以此值为中心，上方管道底部 = pipe_gap_y - PIPE_GAP/2，
 * 下方管道顶部 = pipe_gap_y + PIPE_GAP/2。
 * 每次管道重置时随机生成，范围为 [60, 180)，确保通道不会太靠边。
 * 初始值 120 表示通道在屏幕中央。
 */
static int pipe_gap_y = 120;

/**
 * flappy_score - 当前得分
 * 每当管道完全移出屏幕左侧（pipe_x < -PIPE_WIDTH）时加 1。
 * 游戏重置时归零。
 */
static int flappy_score = 0;

/**
 * flappy_is_playing - 游戏是否正在进行中的标志
 * true  = 游戏正在运行，定时器回调会执行物理模拟
 * false = 游戏暂停/未开始，定时器回调直接返回
 * 用于控制游戏的暂停/恢复状态。
 */
static bool flappy_is_playing = false;

/* ===================== 函数实现 ===================== */

/**
 * @brief 暂停游戏定时器（外部可调用的安全接口）
 *
 * 将游戏状态设为"未在玩"，并暂停 LVGL 定时器。
 * 该函数被声明为非 static，供 ui_manager 等外部模块在
 * 切换屏幕时调用，确保游戏循环不会在后台继续运行。
 *
 * 调用场景：
 *   - 从 Flappy Bird 切换到其他界面时，由 ui_manager 调用
 *   - 游戏结束时，由 game_over() 内部调用
 */
void game_flappy_pause_timer(void) {
    flappy_is_playing = false;               // 标记游戏为非运行状态
    if (game_timer) lv_timer_pause(game_timer); // 如果定时器存在则暂停它
}

/**
 * @brief 游戏结束处理
 *
 * 当碰撞检测返回 true 时调用。执行以下操作：
 *   1. 暂停游戏定时器，停止物理模拟
 *   2. 显示 "Game Over!" 消息及操作提示
 *
 * 提示信息告知玩家：
 *   - "右挥重开" —— 向右挥手可以重新开始游戏
 *   - "左挥退出" —— 向左挥手可以返回游戏列表
 */
static void game_over(void) {
    game_flappy_pause_timer();  // 暂停游戏循环
    // 设置消息文本，\n 为换行符
    lv_label_set_text(flappy_msg_label, "Game Over!\n右挥重开\n左挥退出");
    // 清除隐藏标志，使消息标签可见（游戏开始前被隐藏了）
    lv_obj_clear_flag(flappy_msg_label, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 碰撞检测函数
 *
 * 检测小鸟是否与屏幕边界或管道发生碰撞。检测逻辑分为两部分：
 *
 * 1. 边界碰撞：
 *    - 小鸟顶部超出屏幕上沿（bird_y <= 0）
 *    - 小鸟底部超出屏幕下沿（bird_y + BIRD_HEIGHT >= SCREEN_H）
 *
 * 2. 管道碰撞：
 *    - 首先判断小鸟是否进入了管道所在的 X 轴区间
 *    - 然后判断小鸟的 Y 坐标是否撞到了上管道底部或下管道顶部
 *
 * @return true  发生碰撞（游戏结束）
 * @return false 未发生碰撞（游戏继续）
 */
static bool check_collision(void) {
    /* --- 第1步：屏幕上下边界检测 --- */
    // bird_y 是小鸟左上角的 Y 坐标
    // 如果小鸟顶部碰到屏幕上沿，或底部碰到屏幕下沿，则判定碰撞
    if (bird_y <= 0 || bird_y + BIRD_HEIGHT >= SCREEN_H) return true;

    /* --- 第2步：计算小鸟的水平边界 --- */
    // 小鸟固定在 X=50 的位置（与 ui_game_flappy_init 中 lv_obj_set_pos(bird, 50, bird_y) 对应）
    // bird_right 是小鸟右边缘的 X 坐标
    int bird_right = 50 + BIRD_WIDTH; // 50 + 30 = 80
    int bird_left = 50;               // 小鸟左边缘的 X 坐标

    /* --- 第3步：判断小鸟是否与管道在 X 轴上重叠 --- */
    // pipe_x 是管道左边缘，pipe_x + PIPE_WIDTH 是管道右边缘
    // 只有当小鸟右边缘 > 管道左边缘，且小鸟左边缘 < 管道右边缘时，
    // 说明小鸟在水平方向上与管道重叠
    if (bird_right > pipe_x && bird_left < pipe_x + PIPE_WIDTH) {
        /* --- 第4步：判断小鸟是否撞到了管道（Y 轴方向）--- */
        // pipe_gap_y 是通道中心点，PIPE_GAP/2 是通道半宽
        // 上管道底部 = pipe_gap_y - PIPE_GAP/2
        // 下管道顶部 = pipe_gap_y + PIPE_GAP/2
        // 如果小鸟顶部 < 上管道底部，或小鸟底部 > 下管道顶部，则碰撞
        if (bird_y < pipe_gap_y - PIPE_GAP/2 || bird_y + BIRD_HEIGHT > pipe_gap_y + PIPE_GAP/2) {
            return true;  // 碰撞发生
        }
    }
    return false;  // 无碰撞
}

/**
 * @brief 游戏主循环回调函数（物理引擎）
 *
 * 由 lv_timer 每 33ms 调用一次（约 30FPS），是整个游戏的核心。
 * 每次调用执行以下步骤：
 *   1. 检查游戏是否在运行状态，未运行则直接返回
 *   2. 获取 LVGL 线程锁（确保 UI 操作线程安全）
 *   3. 更新小鸟的物理状态（重力 + 速度 + 位置）
 *   4. 更新管道位置，若管道移出屏幕则重置并加分
 *   5. 更新上下管道控件的位置和尺寸
 *   6. 执行碰撞检测，若碰撞则触发 game_over
 *   7. 释放 LVGL 线程锁
 *
 * @param timer LVGL 定时器指针（本函数未使用，仅为回调签名要求）
 */
static void flappy_game_loop(lv_timer_t * timer) {
    /* 如果游戏未在运行（暂停或未开始），直接返回不执行任何逻辑 */
    if (!flappy_is_playing) return;

    /**
     * lvgl_port_lock(0) - 获取 LVGL 移植层的互斥锁
     * 参数 0 表示不等待（立即返回），如果获取失败则跳过本帧。
     * 这是 ESP-IDF LVGL 移植层的线程安全机制，防止多线程同时操作 UI。
     */
    if (lvgl_port_lock(0)) {

        /* --- 小鸟物理更新 --- */
        // 重力作用：每帧速度增加 GRAVITY（向下加速）
        bird_velocity += GRAVITY;
        // 速度积分到位置：每帧小鸟移动 bird_velocity 个像素
        // 速度为正则向下，为负则向上
        bird_y += bird_velocity;
        // 将计算后的位置应用到小鸟控件上
        lv_obj_set_y(bird, bird_y);

        /* --- 管道位置更新 --- */
        // 管道每帧向左移动 PIPE_SPEED 像素
        pipe_x -= PIPE_SPEED;

        // 如果管道完全移出屏幕左侧（左边缘 < -管道宽度）
        if (pipe_x < -PIPE_WIDTH) {
            // 重置管道到屏幕右侧
            pipe_x = SCREEN_W;
            // 随机生成新的通道中心 Y 坐标
            // rand() % 120 产生 [0, 119] 的随机数，加上 60 后范围为 [60, 179]
            // 确保通道不会太贴近屏幕顶部或底部
            pipe_gap_y = 60 + (rand() % 120);
            // 得分加 1
            flappy_score++;
            // 更新得分标签的显示文本
            lv_label_set_text_fmt(flappy_score_label, "得分: %d", flappy_score);
        }

        /* --- 上管道控件更新 --- */
        // 设置上管道位置：X 与管道组相同，Y 从屏幕顶部(0)开始
        lv_obj_set_pos(pipe_top, pipe_x, 0);
        // 设置上管道高度：从屏幕顶部到通道上边缘
        // 通道上边缘 = pipe_gap_y - PIPE_GAP/2
        lv_obj_set_size(pipe_top, PIPE_WIDTH, pipe_gap_y - PIPE_GAP/2);

        /* --- 下管道控件更新 --- */
        // 设置下管道位置：X 与管道组相同，Y 从通道下边缘开始
        // 通道下边缘 = pipe_gap_y + PIPE_GAP/2
        lv_obj_set_pos(pipe_bottom, pipe_x, pipe_gap_y + PIPE_GAP/2);
        // 设置下管道高度：从通道下边缘到屏幕底部
        lv_obj_set_size(pipe_bottom, PIPE_WIDTH, SCREEN_H - (pipe_gap_y + PIPE_GAP/2));

        /* --- 碰撞检测 --- */
        if (check_collision()) {
            game_over();  // 发生碰撞，游戏结束
        }

        /* 释放 LVGL 互斥锁，允许其他线程操作 UI */
        lvgl_port_unlock();
    }
}

/**
 * @brief Flappy Bird 游戏界面初始化函数
 *
 * 创建并配置游戏界面的所有 UI 元素。采用极简的纯白底 + 黑线框风格。
 * 创建的元素包括：
 *   - 主屏幕容器（白色背景）
 *   - 得分标签（顶部居中）
 *   - 小鸟图片控件（XSBird 精灵图）
 *   - 上管道矩形控件（白色填充 + 黑色边框）
 *   - 下管道矩形控件（白色填充 + 黑色边框）
 *   - 消息提示标签（居中显示操作说明）
 *   - 游戏定时器（33ms 间隔，初始暂停）
 *
 * 该函数在系统启动时由 ui_manager 调用，只执行一次。
 */
void ui_game_flappy_init(void) {
    /* --- 创建主屏幕容器 --- */
    // lv_obj_create(NULL) 创建一个无父对象的屏幕（顶层容器）
    ui_game_flappy_screen = lv_obj_create(NULL);
    // 设置背景色为白色
    lv_obj_set_style_bg_color(ui_game_flappy_screen, lv_color_white(), 0);
    // 设置内边距为 0（去掉默认边距，使子控件可以贴边）
    lv_obj_set_style_pad_all(ui_game_flappy_screen, 0, 0);

    /* --- 创建得分标签 --- */
    flappy_score_label = lv_label_create(ui_game_flappy_screen);
    // 文字颜色为黑色
    lv_obj_set_style_text_color(flappy_score_label, lv_color_black(), 0);
    // 使用自定义中文字体（16px），定义在其他文件中
    lv_obj_set_style_text_font(flappy_score_label, &my_font_cn_16, 0);
    // 初始文本显示 0 分
    lv_label_set_text(flappy_score_label, "得分: 0");
    // 对齐到屏幕顶部居中，Y 方向偏移 15 像素（避免贴顶）
    lv_obj_align(flappy_score_label, LV_ALIGN_TOP_MID, 0, 15);

    /* --- 创建小鸟图片控件 --- */
    // 使用 lv_img 控件显示小鸟精灵图
    bird = lv_img_create(ui_game_flappy_screen);
    // 设置图片源为 XSBird（前面 LV_IMG_DECLARE 声明的资源）
    lv_img_set_src(bird, &XSBird);
    // 小鸟固定在 X=50 的位置，Y 坐标由 bird_y 变量控制
    lv_obj_set_pos(bird, 50, bird_y);
    // 背景透明度设为 0（完全透明，不显示控件本身的背景）
    lv_obj_set_style_bg_opa(bird, 0, 0);
    // 边框宽度设为 0（不显示控件边框，只显示图片本身）
    lv_obj_set_style_border_width(bird, 0, 0);

    /* --- 创建上方管道控件 --- */
    // 用 lv_obj（基础矩形控件）模拟管道
    pipe_top = lv_obj_create(ui_game_flappy_screen);
    // 白色填充（极简风格，管道内部为空白）
    lv_obj_set_style_bg_color(pipe_top, lv_color_white(), 0);
    // 黑色边框，形成可见的管道轮廓
    lv_obj_set_style_border_color(pipe_top, lv_color_black(), 0);
    // 边框宽度 2 像素
    lv_obj_set_style_border_width(pipe_top, 2, 0);
    // 圆角半径设为 0（直角矩形，不要圆角）
    lv_obj_set_style_radius(pipe_top, 0, 0);
    // 内边距设为 0
    lv_obj_set_style_pad_all(pipe_top, 0, 0);

    /* --- 创建下方管道控件 --- */
    // 与上方管道样式完全相同
    pipe_bottom = lv_obj_create(ui_game_flappy_screen);
    lv_obj_set_style_bg_color(pipe_bottom, lv_color_white(), 0);
    lv_obj_set_style_border_color(pipe_bottom, lv_color_black(), 0);
    lv_obj_set_style_border_width(pipe_bottom, 2, 0);
    lv_obj_set_style_radius(pipe_bottom, 0, 0);
    lv_obj_set_style_pad_all(pipe_bottom, 0, 0);

    /* --- 创建消息提示标签 --- */
    flappy_msg_label = lv_label_create(ui_game_flappy_screen);
    // 文字颜色为深灰色（#555555），比纯黑柔和一些
    lv_obj_set_style_text_color(flappy_msg_label, lv_color_hex(0x555555), 0);
    // 文本居中对齐（多行文本时每行都居中）
    lv_obj_set_style_text_align(flappy_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    // 使用自定义中文字体
    lv_obj_set_style_text_font(flappy_msg_label, &my_font_cn_16, 0);
    // 初始提示文本，告知玩家如何操作
    lv_label_set_text(flappy_msg_label, "右挥启动战局\n上挥控制跳跃");
    // 居中显示在屏幕正中央
    lv_obj_align(flappy_msg_label, LV_ALIGN_CENTER, 0, 0);

    /* --- 创建游戏定时器 --- */
    // 创建定时器，每 33ms 调用一次 flappy_game_loop（约 30FPS）
    // 第三个参数 NULL 为用户数据指针（本例不需要）
    game_timer = lv_timer_create(flappy_game_loop, 33, NULL);
    // 立即暂停定时器，等待玩家通过"右挥"手势启动游戏
    lv_timer_pause(game_timer);
}

/**
 * @brief Flappy Bird 游戏的手势命令处理函数（独占手势路由）
 *
 * 当 Flappy Bird 界面处于前台时，所有手势命令都会路由到此函数。
 * 根据游戏状态（是否在玩）对不同手势做出不同响应：
 *
 * 【游戏未运行时】（暂停/未开始/Game Over 状态）：
 *   - UI_CMD_LEFT（左挥）：退出游戏，返回游戏列表界面
 *   - UI_CMD_RIGHT（右挥）：重新开始游戏（重置所有状态并启动定时器）
 *
 * 【游戏运行中】：
 *   - UI_CMD_UP（上挥）：小鸟跳跃（设置向上速度）
 *   - UI_CMD_LEFT（左挥）：暂停游戏并返回游戏列表
 *
 * @param cmd 手势命令枚举值，由 ui_manager 传入
 */
void game_flappy_screen_handle_cmd(ui_cmd_t cmd) {
    if (!flappy_is_playing) {
        /* ====== 游戏未运行状态下的命令处理 ====== */

        if (cmd == UI_CMD_LEFT) {
            /* 左挥：退出当前游戏，返回游戏列表 */
            // 声明外部函数 switch_to_screen（定义在 ui_manager.c 中）
            extern void switch_to_screen(ui_screen_state_t target);
            // 切换到游戏列表界面
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            /* 右挥：开始/重新开始游戏 */

            /* 重置所有游戏状态变量到初始值 */
            bird_y = 100;               // 小鸟 Y 坐标重置到 100（略低于中央）
            bird_velocity = 0;          // 垂直速度归零
            pipe_x = SCREEN_W;          // 管道重置到屏幕右侧
            flappy_score = 0;           // 得分归零

            /* 重置 UI 显示 */
            // 隐藏消息提示标签（"右挥启动战局"或 "Game Over" 提示）
            lv_obj_add_flag(flappy_msg_label, LV_OBJ_FLAG_HIDDEN);
            // 重置得分显示为 0
            lv_label_set_text(flappy_score_label, "得分: 0");
            // 将上管道移到屏幕右侧顶部
            lv_obj_set_pos(pipe_top, SCREEN_W, 0);
            // 将下管道移到屏幕右侧底部
            lv_obj_set_pos(pipe_bottom, SCREEN_W, SCREEN_H);

            /* 启动游戏 */
            flappy_is_playing = true;       // 标记游戏为运行状态
            lv_timer_resume(game_timer);    // 恢复定时器，开始游戏循环
        }
    }
    else {
        /* ====== 游戏运行中的命令处理 ====== */

        if (cmd == UI_CMD_UP) {
            /* 上挥：小鸟跳跃 */
            // 将小鸟的垂直速度设为 JUMP_STRENGTH（-12，即向上运动）
            // 之后每帧受重力影响逐渐减速、停止、再加速下落
            bird_velocity = JUMP_STRENGTH;
        }
        else if (cmd == UI_CMD_LEFT) {
            /* 左挥：暂停游戏并返回游戏列表 */
            game_flappy_pause_timer();      // 暂停游戏定时器
            // 声明并调用外部函数，切换回游戏列表界面
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
    }
}
