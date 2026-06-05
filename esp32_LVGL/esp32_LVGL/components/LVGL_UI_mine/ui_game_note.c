/**
 * @file ui_game_note.c
 * @brief 声控跑酷小游戏 —— "音符君冲刺"
 *
 * 游戏玩法说明：
 *   玩家通过麦克风声音大小来控制游戏中的小方块"音符君"。
 *   - 声音低于 DB_SILENCE（沉默）：音符君静止不动，障碍物也不滚动。
 *   - 声音高于 DB_SILENCE 但低于 DB_JUMP（轻哼/说话）：音符君慢速前进，障碍物缓慢滚动。
 *   - 声音高于 DB_JUMP（尖叫/大喊）：音符君触发跳跃，同时障碍物高速滚动。
 *   如果音符君碰到树木障碍物，则游戏结束。
 *
 * 手势操作：
 *   - 游戏未开始时：右挥开始游戏，左挥返回游戏列表。
 *   - 游戏进行中：左挥强制退出返回游戏列表。
 */

/* ======================== 头文件包含 ======================== */

#include "ui_globals.h"       // 全局变量与共享资源声明（如字体 my_font_cn_16 等）
#include "ui_manager.h"       // UI 管理器，提供 ui_cmd_t 命令枚举和屏幕状态枚举
#include "esp_lvgl_port.h"    // ESP-IDF 的 LVGL 移植层，提供 lvgl_port_lock/unlock 线程安全接口
#include <stdlib.h>           // 标准库，用于 rand() 随机数生成

/* ======================== 图片资源声明 ======================== */

/**
 * LV_IMG_DECLARE 宏用于声明一个外部定义的 LVGL 图片资源。
 * 此处声明 "tree" 图片，该图片在其他文件中通过 LV_IMG_DEF 定义，
 * 用作地面上的树木障碍物贴图。
 */
LV_IMG_DECLARE(tree);

/* ======================== 全局/静态 UI 对象 ======================== */

/**
 * ui_game_note_screen - 本游戏界面的顶层屏幕对象（全局可见）。
 * 其他模块（如 ui_manager）通过此指针来切换到本游戏界面。
 */
lv_obj_t * ui_game_note_screen;

/**
 * player - "音符君"玩家角色对象。
 * 这是一个白色小方块（带圆角），代表玩家控制的角色。
 * 其水平位置固定在 x=50，垂直位置通过 player_y 动态更新实现跳跃效果。
 */
static lv_obj_t * player;

/**
 * obstacle - 地面障碍物对象（树木图片）。
 * 从屏幕右侧不断向左滚动，玩家需要跳跃躲避。
 * 使用 tree 图片资源渲染，而非简单的矩形色块。
 */
static lv_obj_t * obstacle;

/**
 * floor_line - 地平线对象。
 * 一条白色水平线，标识地面位置（y = FLOOR_Y = 180）。
 * 所有游戏元素均以此线为参考基准。
 */
static lv_obj_t * floor_line;

/**
 * score_label - 得分显示标签。
 * 显示在屏幕顶部居中，实时更新当前得分。
 * 格式为 "得分: X"，每当障碍物完整移出屏幕左侧时加 1 分。
 */
static lv_obj_t * score_label;

/**
 * msg_label - 状态提示信息标签。
 * 游戏开始前显示操作说明，游戏结束后显示"游戏结束"及后续操作提示。
 * 游戏进行中隐藏此标签。
 */
static lv_obj_t * msg_label;

/**
 * db_indicator - 屏幕顶部分贝可视化能量条。
 * 一个绿色水平条，宽度随当前分贝值动态变化，
 * 让玩家直观看到自己声音的大小，作为声音输入的视觉反馈。
 */
static lv_obj_t * db_indicator;

/**
 * note_timer - 游戏主循环定时器。
 * 以约 33ms 间隔（约 30 FPS）触发 note_game_loop 回调函数，
 * 驱动整个游戏的物理模拟、碰撞检测和画面刷新。
 * 游戏暂停时通过 lv_timer_pause 暂停，恢复时通过 lv_timer_resume 恢复。
 */
static lv_timer_t * note_timer = NULL;

/* ======================== 屏幕与游戏常量宏定义 ======================== */

#define SCREEN_W        240     // 屏幕宽度（像素），匹配 ESP32 驱动的显示屏物理分辨率
#define SCREEN_H        240     // 屏幕高度（像素）
#define FLOOR_Y         180     // 地平线的 Y 坐标（像素），即地面在屏幕上的垂直位置
#define PLAYER_SIZE     16      // 玩家方块的边长（像素），角色为正方形

/**
 * 障碍物尺寸，需与 tree 图片的实际像素尺寸精确匹配，
 * 以确保碰撞检测的准确性。树宽 32 像素，树高 26 像素。
 */
#define OBS_WIDTH       32      // 树木障碍物的宽度（像素）
#define OBS_HEIGHT      26      // 树木障碍物的高度（像素）

/**
 * 声控核心阈值 —— 这两个值决定了声音控制的灵敏度。
 * 可根据实际麦克风硬件的灵敏度和环境噪声水平进行微调。
 *
 * DB_SILENCE（沉默阈值）：
 *   当前分贝低于此值时，判定为"沉默"状态，音符君不移动，障碍物不滚动。
 *   设为 55 dB 是为了避免环境底噪被误判为有效输入。
 *
 * DB_JUMP（跳跃阈值）：
 *   当前分贝高于此值时，判定为"尖叫/大喊"状态，触发跳跃动作，
 *   同时障碍物以最高速度滚动。
 *   设为 72 dB 表示需要较大的声音才能触发跳跃。
 */
#define DB_SILENCE      55      // 低于此分贝判定为沉默，走不动
#define DB_JUMP         72      // 高于此分贝判定为尖叫，触发跳跃

/* ======================== 游戏运行时状态变量 ======================== */

/**
 * player_y - 音符君的当前 Y 坐标（左上角）。
 * 初始值为 FLOOR_Y - PLAYER_SIZE，即角色底部刚好落在地平线上。
 * 跳跃时此值会减小（向上移动），受重力影响后逐渐增大（下落）。
 */
static int player_y = FLOOR_Y - PLAYER_SIZE;

/**
 * player_vel_y - 音符君的垂直速度（像素/帧）。
 * 正值表示向下运动，负值表示向上运动。
 * 跳跃时赋值为 -12（向上），每帧受重力 +1 逐渐减速直至反向下落。
 * 落地时重置为 0。
 */
static int player_vel_y = 0;

/**
 * obs_x - 障碍物（树木）的当前 X 坐标（左上角）。
 * 初始值为 SCREEN_W（240），即从屏幕右侧外开始。
 * 每帧根据 scroll_speed 向左移动，移出屏幕左侧后重新在右侧生成。
 */
static int obs_x = SCREEN_W;

/**
 * game_score - 当前游戏得分。
 * 每当一个障碍物完整移出屏幕左侧时加 1 分。
 * 游戏结束时保留最终得分，重新开始时重置为 0。
 */
static int game_score = 0;

/**
 * current_db - 当前麦克风采集到的分贝值。
 * 由外部模块（串口或音频采集任务）通过 game_note_pass_db() 函数实时更新。
 * 游戏主循环读取此值来决定移动速度和是否触发跳跃。
 */
static int current_db = 0;

/**
 * is_playing - 游戏是否正在进行中的标志。
 * true  = 游戏进行中，定时器运行，物理模拟和碰撞检测生效。
 * false = 游戏未开始或已结束，定时器暂停，等待玩家重新开始。
 */
static bool is_playing = false;

/**
 * is_grounded - 音符君是否站在地面上的标志。
 * true  = 角色在地面上，可以触发跳跃。
 * false = 角色在空中（跳跃过程中），不能再次跳跃（防止二段跳）。
 * 落地后自动恢复为 true。
 */
static bool is_grounded = true;

/* ======================== 函数实现 ======================== */

/**
 * @brief 高频分贝注入函数 —— 供外部模块（串口/音频采集任务）调用
 *
 * 此函数由外部模块以较高频率（如每次麦克风采样后）调用，
 * 将实时分贝值注入到游戏中。为了减少输入延迟，跳跃触发逻辑
 * 直接在此函数中判断，而非等到下一帧游戏主循环才处理。
 *
 * @param val 当前分贝值（由麦克风采集模块计算得出）
 *
 * 实时性设计说明：
 *   如果仅在 note_game_loop 中检测跳跃，最坏情况下需要等待 33ms
 *   才能响应一次尖叫，造成明显延迟。将跳跃触发放在注入函数中，
 *   可以在声音到达的瞬间立即响应，提升游戏体验的流畅度。
 */
void game_note_pass_db(int val) {
    current_db = val;
    /**
     * 实时跳跃触发判断：
     *   条件1: is_playing  -- 游戏正在进行中（非暂停/结束状态）
     *   条件2: val >= DB_JUMP -- 当前分贝超过跳跃阈值（玩家在尖叫）
     *   条件3: is_grounded  -- 角色站在地面上（防止空中二段跳）
     *   三个条件同时满足时，立即赋予向上的跳跃速度 -12，并标记角色离开地面。
     */
    if (is_playing && val >= DB_JUMP && is_grounded) {
        player_vel_y = -12;        // 赋予向上的跳跃速度（负值 = 向上）
        is_grounded = false;       // 标记角色已离开地面，防止空中再次跳跃
    }
}

/**
 * @brief 游戏结束处理函数
 *
 * 当检测到碰撞（音符君碰到树木）时调用此函数。
 * 执行以下操作：
 *   1. 将 is_playing 标志置为 false，停止游戏逻辑。
 *   2. 暂停游戏主循环定时器，停止物理模拟。
 *   3. 显示"游戏结束"提示信息，告知玩家可右挥重开或左挥退出。
 */
static void game_over(void) {
    is_playing = false;                        // 标记游戏结束，note_game_loop 将不再执行任何逻辑
    if (note_timer) lv_timer_pause(note_timer); // 暂停定时器，节省 CPU 资源
    lv_label_set_text(msg_label, "游戏结束!\n右挥重新开始\n左挥退出"); // 设置结束提示文本
    lv_obj_clear_flag(msg_label, LV_OBJ_FLAG_HIDDEN); // 显示提示标签（清除隐藏标志）
}

/**
 * @brief 矩形碰撞检测函数 —— AABB（轴对齐包围盒）算法
 *
 * 判断玩家角色（音符君）与障碍物（树木）的矩形包围盒是否重叠。
 * 采用经典的 AABB 碰撞检测：两个矩形在 X 轴和 Y 轴上都存在重叠时判定为碰撞。
 *
 * @return true  发生碰撞（游戏结束条件）
 * @return false 未发生碰撞
 *
 * 坐标系说明：
 *   LVGL 坐标系原点在左上角，X 轴向右增大，Y 轴向下增大。
 *   矩形通过左上角坐标 (left, top) 和右下角坐标 (right, bottom) 描述。
 */
static bool check_collision(void) {
    /* --- 计算玩家角色的包围盒 --- */
    /* 玩家水平位置固定在 x=50，宽度为 PLAYER_SIZE */
    int p_left = 50, p_right = 50 + PLAYER_SIZE;
    /* 玩家垂直位置由 player_y 动态决定，高度为 PLAYER_SIZE */
    int p_top = player_y, p_bottom = player_y + PLAYER_SIZE;

    /* --- 计算障碍物（树木）的包围盒 --- */
    /* 障碍物水平位置由 obs_x 动态决定，宽度为 OBS_WIDTH */
    int o_left = obs_x, o_right = obs_x + OBS_WIDTH;
    /* 障碍物底部紧贴地平线（FLOOR_Y），顶部为 FLOOR_Y - OBS_HEIGHT */
    int o_top = FLOOR_Y - OBS_HEIGHT, o_bottom = FLOOR_Y;

    /**
     * AABB 碰撞判定逻辑：
     *   第一步：检查 X 轴方向是否重叠
     *     玩家右边缘 > 障碍物左边缘  且  玩家左边缘 < 障碍物右边缘
     *   第二步：检查 Y 轴方向是否重叠
     *     玩家下边缘 > 障碍物上边缘  且  玩家上边缘 < 障碍物下边缘
     *   两轴均重叠时，两个矩形相交，判定为碰撞。
     */
    if (p_right > o_left && p_left < o_right) {
        if (p_bottom > o_top && p_top < o_bottom) {
            return true;   // 发生碰撞
        }
    }
    return false;          // 未发生碰撞
}

/**
 * @brief 游戏主循环回调函数 —— 约 30 FPS 驱动整个游戏
 *
 * 此函数由 note_timer 定时器每 33ms 调用一次，负责：
 *   1. 刷新顶部的分贝可视化能量条（视觉反馈）
 *   2. 执行垂直方向的物理模拟（重力 + 跳跃）
 *   3. 根据分贝值计算水平滚动速度（声控核心逻辑）
 *   4. 移动障碍物并处理得分
 *   5. 执行碰撞检测
 *
 * 线程安全说明：
 *   由于 LVGL 不是线程安全的，所有 UI 操作必须在持有 LVGL 锁的情况下执行。
 *   使用 lvgl_port_lock(0) 尝试获取锁（0 表示不等待，立即返回），
 *   获取成功才执行 UI 更新，获取失败则跳过本帧。
 *
 * @param timer 定时器指针（本函数未使用此参数）
 */
static void note_game_loop(lv_timer_t * timer) {
    /* 游戏未开始或已结束时，直接返回，不执行任何逻辑 */
    if (!is_playing) return;

    /**
     * lvgl_port_lock(0) 尝试获取 LVGL 互斥锁。
     * 参数 0 表示非阻塞模式：如果锁被其他线程占用，立即返回 false，不等待。
     * 获取成功返回 true，此时可以安全操作 LVGL 对象。
     */
    if (lvgl_port_lock(0)) {

        /* ---- 步骤 1: 刷新顶部分贝可视化能量条 ---- */
        /**
         * 将分贝值转换为能量条宽度：
         *   - 分贝 <= 40 时，能量条宽度为 0（太安静，不显示）
         *   - 分贝 > 40 时，宽度 = (分贝 - 40) * 2 像素
         *   - 例：分贝 55 → 宽度 30px；分贝 72 → 宽度 64px；分贝 80 → 宽度 80px
         * 高度固定为 4 像素，形成一个细长的顶部能量条效果。
         */
        int bar_w = (current_db > 40) ? (current_db - 40) * 2 : 0;
        lv_obj_set_size(db_indicator, bar_w, 4);

        /* ---- 步骤 2: 垂直方向物理引擎（重力机制） ---- */
        /**
         * 仅当角色不在地面上时（is_grounded == false）执行物理模拟：
         *   - 每帧 player_vel_y 增加 1（模拟重力加速度，约 1 像素/帧²）
         *   - player_y 根据当前速度更新位置
         *   - 当 player_y 回到或超过地面位置时，修正为地面位置，
         *     速度归零，标记 is_grounded = true
         *
         * 跳跃轨迹示意：
         *   初始 vel_y = -12（向上飞）
         *   每帧 +1：-12, -11, -10, ..., -1, 0（最高点）, +1, +2, ...
         *   角色先上升后下降，形成抛物线跳跃弧线。
         */
        if (!is_grounded) {
            player_vel_y += 1;     // 重力加速度：每帧速度增加 1（向下加速）
            player_y += player_vel_y; // 根据当前速度更新垂直位置

            /**
             * 落地检测：
             *   当角色的底部（player_y + PLAYER_SIZE）到达或超过地平线（FLOOR_Y）时，
             *   说明角色已经落地。需要：
             *     1. 修正 player_y 为刚好在地面上的值，防止陷入地下
             *     2. 垂直速度归零
             *     3. 标记 is_grounded = true，允许下一次跳跃
             */
            if (player_y >= FLOOR_Y - PLAYER_SIZE) { // 落回地面
                player_y = FLOOR_Y - PLAYER_SIZE;    // 修正位置：底部紧贴地平线
                player_vel_y = 0;                     // 速度归零
                is_grounded = true;                   // 标记已落地
            }
            lv_obj_set_y(player, player_y); // 更新 LVGL 对象的 Y 坐标，刷新画面
        }

        /* ---- 步骤 3: 水平方向滚动速度控制（声控核心算法） ---- */
        /**
         * 根据当前分贝值将滚动速度分为三档：
         *
         * 档位 1 - 沉默（current_db <= DB_SILENCE 即 <= 55 dB）：
         *   scroll_speed = 0，障碍物不移动，游戏世界静止。
         *   此时玩家即使想跳也跳不起来（因为跳跃需要 DB_JUMP 即 72 dB）。
         *
         * 档位 2 - 轻哼/正常说话（DB_SILENCE < current_db < DB_JUMP 即 55~72 dB）：
         *   scroll_speed = 3，障碍物缓慢向左滚动。
         *   玩家在"慢跑"前进。
         *
         * 档位 3 - 尖叫/大喊（current_db >= DB_JUMP 即 >= 72 dB）：
         *   scroll_speed = 6，障碍物高速向左滚动。
         *   同时触发跳跃（在 game_note_pass_db 中已处理），玩家在"狂奔+跳跃"。
         */
        int scroll_speed = 0;
        if (current_db > DB_SILENCE) {
            if (current_db >= DB_JUMP) {
                scroll_speed = 6;  // 尖叫时：不仅在跳，地面也在狂奔（高速滚动）
            } else {
                scroll_speed = 3;  // 轻哼时：小步漫跑（低速滚动）
            }
        } else {
            scroll_speed = 0;      // 沉默时：静止在原地（不滚动）
        }

        /* ---- 步骤 4: 地面障碍物移动与得分 ---- */
        /**
         * 仅当 scroll_speed > 0 时才移动障碍物（即有声音输入时）。
         * 障碍物每帧向左移动 scroll_speed 个像素。
         *
         * 当障碍物完全移出屏幕左侧（obs_x < -OBS_WIDTH）时：
         *   1. 在屏幕右侧重新生成一个新的障碍物位置
         *      使用 rand() % 40 添加 0~39 像素的随机偏移，
         *      使每次障碍物出现的位置略有不同，增加游戏的可玩性。
         *   2. 得分加 1，并更新计分板显示。
         */
        if (scroll_speed > 0) {
            obs_x -= scroll_speed; // 障碍物向左移动
            if (obs_x < -OBS_WIDTH) {
                obs_x = SCREEN_W + (rand() % 40); // 移出屏幕后在右侧外随机位置重新生成
                game_score++;                       // 得分加 1
                lv_label_set_text_fmt(score_label, "得分: %d", game_score); // 更新计分板文本
            }
            lv_obj_set_x(obstacle, obs_x); // 更新 LVGL 对象的 X 坐标，刷新画面
        }

        /* ---- 步骤 5: 碰撞判定 ---- */
        /**
         * 每帧检测玩家与障碍物是否碰撞。
         * 如果发生碰撞（check_collision 返回 true），调用 game_over() 结束游戏。
         */
        if (check_collision()) {
            game_over();
        }

        /**
         * lvgl_port_unlock() 释放 LVGL 互斥锁。
         * 与前面的 lvgl_port_lock(0) 配对使用，确保线程安全。
         * 释放后其他线程可以操作 LVGL 对象。
         */
        lvgl_port_unlock();
    }
}

/**
 * @brief 游戏界面初始化函数 —— 创建所有 UI 元素并初始化定时器
 *
 * 此函数在游戏界面首次加载时由 UI 管理器调用，仅执行一次。
 * 负责创建以下 UI 元素：
 *   - 屏幕对象（黑色背景）
 *   - 分贝能量条（屏幕顶部绿色条）
 *   - 得分标签（屏幕顶部居中白色文字）
 *   - 地平线（白色水平线）
 *   - 玩家角色（白色带圆角小方块）
 *   - 障碍物（树木图片）
 *   - 状态提示标签（居中灰色文字）
 *   - 游戏主循环定时器（初始暂停状态）
 */
void ui_game_note_init(void) {
    /* --- 创建顶层屏幕对象 --- */
    /**
     * lv_obj_create(NULL) 创建一个无父对象的屏幕。
     * 设置黑色背景和零内边距，作为整个游戏界面的根容器。
     */
    ui_game_note_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_game_note_screen, lv_color_black(), 0); // 黑色背景，模拟游戏夜空/简洁风格
    lv_obj_set_style_pad_all(ui_game_note_screen, 0, 0);                // 零内边距，充分利用屏幕空间

    /* --- 分贝状态指示器（顶部能量条） --- */
    /**
     * 一个绿色水平条，位于屏幕最顶部 (0, 0)。
     * 初始宽度为 0（不可见），游戏循环中根据分贝值动态调整宽度。
     * 无边框无内边距，仅显示纯色背景。
     */
    db_indicator = lv_obj_create(ui_game_note_screen);
    lv_obj_set_pos(db_indicator, 0, 0);                                      // 位于屏幕左上角
    lv_obj_set_size(db_indicator, 0, 4);                                     // 初始宽度 0，高度 4 像素
    lv_obj_set_style_bg_color(db_indicator, lv_color_hex(0x00FF00), 0);      // 绿色，与"能量/活力"感匹配
    lv_obj_set_style_border_width(db_indicator, 0, 0);                       // 无边框
    lv_obj_set_style_pad_all(db_indicator, 0, 0);                            // 无内边距

    /* --- 计分板 --- */
    /**
     * 白色文字标签，使用中文 16px 字体，显示 "得分: 0"。
     * 对齐到屏幕顶部居中，向下偏移 15 像素，
     * 避免与顶部能量条重叠。
     */
    score_label = lv_label_create(ui_game_note_screen);
    lv_obj_set_style_text_color(score_label, lv_color_white(), 0);           // 白色文字，在黑色背景上清晰可见
    lv_obj_set_style_text_font(score_label, &my_font_cn_16, 0);             // 使用自定义中文字体
    lv_label_set_text(score_label, "得分: 0");                               // 初始得分文本
    lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 15);                     // 顶部居中，向下偏移 15px

    /* --- 地平线 --- */
    /**
     * 一条白色水平线，宽度为屏幕全宽，高度 2 像素。
     * 位于 y = FLOOR_Y (180) 处，将游戏区域分为"天空"和"地面"。
     * 无边框无内边距，仅显示纯色背景。
     */
    floor_line = lv_obj_create(ui_game_note_screen);
    lv_obj_set_size(floor_line, SCREEN_W, 2);                                // 全宽，2 像素高
    lv_obj_set_pos(floor_line, 0, FLOOR_Y);                                  // 位于地平线高度
    lv_obj_set_style_bg_color(floor_line, lv_color_white(), 0);              // 白色线条
    lv_obj_set_style_border_width(floor_line, 0, 0);                         // 无边框
    lv_obj_set_style_pad_all(floor_line, 0, 0);                              // 无内边距

    /* --- 玩家小方块（音符君） --- */
    /**
     * 白色带圆角的小方块，代表玩家角色。
     * 水平位置固定在 x=50，垂直位置由 player_y 控制。
     * 圆角半径 2 像素，使方块看起来更柔和。
     * 初始位置：底部紧贴地平线。
     */
    player = lv_obj_create(ui_game_note_screen);
    lv_obj_set_size(player, PLAYER_SIZE, PLAYER_SIZE);                       // 16x16 像素的正方形
    lv_obj_set_pos(player, 50, player_y);                                    // x=50（固定），y=player_y（动态）
    lv_obj_set_style_bg_color(player, lv_color_white(), 0);                  // 白色方块
    lv_obj_set_style_border_width(player, 0, 0);                             // 无边框
    lv_obj_set_style_radius(player, 2, 0);                                   // 圆角半径 2px
    lv_obj_set_style_pad_all(player, 0, 0);                                  // 无内边距

    /* --- 障碍物（树木贴图） --- */
    /**
     * 使用 tree 图片资源作为障碍物，替代简单的矩形色块。
     * 位置由 obs_x 控制水平移动，垂直位置固定在 FLOOR_Y - OBS_HEIGHT，
     * 确保树木底部紧贴地平线。
     * 背景设为透明，避免图片周围的白色方框。
     */
    obstacle = lv_img_create(ui_game_note_screen);
    lv_img_set_src(obstacle, &tree);                                          // 设置图片源为 tree 资源
    lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT);                  // 底部紧贴地平线
    lv_obj_set_style_bg_opa(obstacle, 0, 0);                                 // 背景完全透明

    /* --- 提示语标签 --- */
    /**
     * 灰色居中文字，使用中文 16px 字体。
     * 显示游戏操作说明，告知玩家声音控制的规则。
     * 位于屏幕正中央，向上偏移 10 像素。
     * 游戏开始后此标签被隐藏，游戏结束后重新显示结束提示。
     */
    msg_label = lv_label_create(ui_game_note_screen);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(0xAAAAAA), 0);       // 灰色文字 (#AAAAAA)
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);         // 文字居中对齐（支持多行）
    lv_obj_set_style_text_font(msg_label, &my_font_cn_16, 0);               // 使用自定义中文字体
    lv_label_set_text(msg_label, "右挥启动声控\n声音低: 前进\n尖叫: 跳跃"); // 操作说明文本
    lv_obj_align(msg_label, LV_ALIGN_CENTER, 0, -10);                       // 屏幕居中，向上偏移 10px

    /* --- 游戏主循环定时器 --- */
    /**
     * 创建一个 33ms 周期的定时器，约等于 30 FPS（1000ms / 33ms ≈ 30.3）。
     * 回调函数为 note_game_loop，每次触发时执行一帧游戏逻辑。
     * 创建后立即暂停，等待玩家通过右挥手势启动游戏。
     */
    note_timer = lv_timer_create(note_game_loop, 33, NULL); // 33ms 周期 ≈ 30FPS
    lv_timer_pause(note_timer);                              // 初始状态：暂停，等待游戏启动
}

/**
 * @brief 游戏界面的命令处理函数 —— 响应手势操作
 *
 * 由 UI 管理器在检测到手势输入时调用，将手势转换为游戏控制命令。
 *
 * @param cmd 手势命令，取值为：
 *   - UI_CMD_LEFT  : 左挥手势
 *   - UI_CMD_RIGHT : 右挥手势
 *
 * 命令处理逻辑：
 *
 * 【游戏未开始 / 已结束状态】（is_playing == false）：
 *   - 左挥（UI_CMD_LEFT）：
 *     退出当前游戏界面，返回游戏列表屏幕（SCREEN_GAME_LIST）。
 *     使用 switch_to_screen() 函数进行屏幕切换。
 *
 *   - 右挥（UI_CMD_RIGHT）：
 *     重置所有游戏状态变量并启动游戏：
 *       1. 玩家位置重置到地面
 *       2. 垂直速度归零
 *       3. 障碍物重置到屏幕右侧
 *       4. 得分清零
 *       5. 隐藏提示标签
 *       6. 更新计分板显示
 *       7. 刷新玩家和障碍物的 LVGL 对象位置
 *       8. 设置 is_playing = true
 *       9. 恢复（启动）游戏定时器
 *
 * 【游戏进行中状态】（is_playing == true）：
 *   - 左挥（UI_CMD_LEFT）：
 *     强制退出游戏，暂停定时器，返回游戏列表屏幕。
 *     右挥在游戏进行中无特殊处理，因为跳跃由声音控制。
 */
void game_note_screen_handle_cmd(ui_cmd_t cmd) {
    if (!is_playing) {
        /* ---- 游戏未开始或已结束状态 ---- */

        if (cmd == UI_CMD_LEFT) {
            /**
             * 左挥：返回游戏列表。
             * switch_to_screen() 是外部定义的屏幕切换函数，
             * 声明为 extern 因为它定义在其他源文件中（ui_manager.c）。
             * SCREEN_GAME_LIST 是游戏列表屏幕的状态枚举值。
             */
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);
        }
        else if (cmd == UI_CMD_RIGHT) {
            /**
             * 右挥：重置并开启游戏。
             * 将所有游戏状态变量恢复到初始值，
             * 更新所有 UI 元素的位置和显示内容，
             * 然后启动游戏定时器开始游戏循环。
             */
            player_y = FLOOR_Y - PLAYER_SIZE;   // 玩家位置重置：底部紧贴地平线
            player_vel_y = 0;                     // 垂直速度归零
            obs_x = SCREEN_W;                     // 障碍物重置到屏幕右侧外
            game_score = 0;                       // 得分清零
            is_grounded = true;                   // 标记角色在地面上
            current_db = 0;                       // 分贝值归零

            lv_obj_add_flag(msg_label, LV_OBJ_FLAG_HIDDEN);  // 隐藏操作提示标签
            lv_label_set_text(score_label, "得分: 0");        // 重置计分板文本
            lv_obj_set_pos(player, 50, player_y);             // 刷新玩家位置
            lv_obj_set_pos(obstacle, obs_x, FLOOR_Y - OBS_HEIGHT); // 刷新障碍物位置

            is_playing = true;                    // 标记游戏开始
            lv_timer_resume(note_timer);          // 恢复（启动）游戏定时器
        }
    } else {
        /* ---- 游戏进行中状态 ---- */

        if (cmd == UI_CMD_LEFT) {
            /**
             * 左挥：游戏中强制退出。
             * 立即停止游戏状态，暂停定时器，
             * 返回游戏列表屏幕。
             * 注意：游戏中右挥不处理，因为跳跃完全由声音控制。
             */
            is_playing = false;                   // 标记游戏停止
            lv_timer_pause(note_timer);           // 暂停游戏定时器
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_GAME_LIST);   // 返回游戏列表
        }
    }
}
