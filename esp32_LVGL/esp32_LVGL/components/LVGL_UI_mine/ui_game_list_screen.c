/**
 * @file ui_game_list_screen.c
 * @brief 游戏列表界面的实现文件
 *
 * 本文件实现了游戏中心的列表选择界面，用户可以在该界面中浏览并选择不同的游戏。
 * 界面包含一个标题标签和一个滚轮选择器（Roller），用户通过上/下手势切换游戏选项，
 * 通过左/右手势进行返回主菜单或进入对应游戏的操作。
 */

#include "ui_globals.h"         /* 全局变量和宏定义头文件，包含UI状态、颜色、字体等公共资源 */
#include "esp_lvgl_port.h"      /* ESP-IDF的LVGL移植层头文件，提供LVGL与ESP硬件的桥接功能 */
#include "ui_manager.h"         /* UI管理器头文件，包含屏幕状态枚举、命令类型定义和切换接口 */

/* ========================================================================
 * 宏定义
 * ======================================================================== */

/**
 * @def GAME_ITEM_COUNT
 * @brief 游戏列表中的游戏总数
 *
 * 当前游戏列表包含4个游戏：
 *   0 - 赛博跑酷（Cyber Runner）
 *   1 - 经典2048
 *   2 - 像素鸟（Flappy Bird）
 *   3 - 八分音符酱（音符类游戏）
 * 每当新增或删除游戏时，需要同步修改此宏定义以及下方滚轮选项字符串。
 */
#define GAME_ITEM_COUNT 5

/* ========================================================================
 * 全局变量
 * ======================================================================== */

/**
 * @var ui_game_list_screen
 * @brief 游戏列表界面的屏幕根对象（全局）
 *
 * 这是游戏列表界面的顶层LVGL对象（screen），通过lv_scr_load()可以切换到该界面。
 * 声明为全局变量，以便UI管理器可以直接访问和加载此屏幕。
 */
lv_obj_t * ui_game_list_screen;

/**
 * @var game_roller
 * @brief 游戏选择滚轮控件（文件内静态变量）
 *
 * 使用static修饰，表示该变量仅在本文件内可见（文件作用域）。
 * 这是一个LVGL的Roller控件，用于展示游戏列表并允许用户滚动选择。
 * 用户通过上下手势滚动滚轮，通过右键确认进入选中的游戏。
 */
static lv_obj_t * game_roller;

/* ========================================================================
 * 函数实现
 * ======================================================================== */

/**
 * @brief 初始化游戏列表界面
 *
 * 该函数负责创建并配置游戏列表界面的所有UI元素，包括：
 *   1. 创建屏幕根对象并设置黑色背景
 *   2. 创建顶部标题标签，显示"游戏中心"
 *   3. 创建滚轮选择器，列出所有可选游戏
 *   4. 配置滚轮的样式（颜色、字体、边框等）
 *
 * 该函数通常在系统启动时由UI管理器调用一次，完成界面的静态初始化。
 * 初始化完成后，界面并不会自动显示，需要通过switch_to_screen()切换到该界面。
 */
void ui_game_list_screen_init(void) {

    /* ---- 第一步：创建屏幕根对象 ---- */
    /**
     * lv_obj_create(NULL) 创建一个没有父对象的屏幕对象。
     * 传入NULL表示创建的是一个独立的顶层屏幕（screen），而不是某个父对象的子控件。
     * 所有其他UI元素都将作为该屏幕的子对象被创建。
     */
    ui_game_list_screen = lv_obj_create(NULL);

    /**
     * 设置屏幕背景颜色为纯黑色（lv_color_black()）。
     * 第三个参数0表示设置默认样式的属性（而非特定的部件或状态）。
     * 黑色背景在AR眼镜等头戴设备上可以节省功耗并提供更好的对比度。
     */
    lv_obj_set_style_bg_color(ui_game_list_screen, lv_color_black(), 0);

    /* ---- 第二步：创建顶部标题标签 ---- */
    /**
     * 在游戏列表屏幕上创建一个标签（label）控件，用于显示界面标题。
     * 标签是LVGL中最常用的文本显示控件。
     */
    lv_obj_t * label_title = lv_label_create(ui_game_list_screen);

    /**
     * 设置标题标签使用自定义中文字体（my_font_cn_16，16像素的中文字体）。
     * LVGL默认只包含ASCII字体，中文字符需要额外的字体文件支持。
     * 该字体在项目中通过工具预先生成并注册。
     */
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0);

    /**
     * 设置标题文字颜色为青色（0x00FFFF），即RGB(0, 255, 255)。
     * 青色在深色背景上具有良好的视觉辨识度，常用于科技/赛博朋克风格的界面设计。
     */
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FFFF), 0);

    /**
     * 设置标签显示的文本内容为"游戏中心"。
     * 该文本将使用前面设置的中文字体进行渲染。
     */
    lv_label_set_text(label_title, "游戏中心");

    /**
     * 将标题标签对齐到屏幕顶部居中位置，并向下偏移20像素。
     * LV_ALIGN_TOP_MID 表示以父对象（屏幕）的顶部中点为参考点。
     * x偏移为0（水平居中），y偏移为20（向下偏移20像素），避免紧贴屏幕顶端。
     */
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    /* ---- 第三步：创建游戏列表滚轮选择器 ---- */
    /**
     * 创建一个Roller（滚轮）控件，用于展示游戏列表。
     * Roller是LVGL提供的一个类似于iOS滚轮选择器的控件，
     * 用户可以上下滚动选择列表中的某一项。
     * 它作为game_list_screen的子对象被创建。
     */
    game_roller = lv_roller_create(ui_game_list_screen);

    /**
     * 设置滚轮的选项列表。各选项之间用换行符"\n"分隔。
     * LV_ROLLER_MODE_NORMAL 表示普通模式（非无限循环模式），
     * 即滚动到列表首尾时会停止，不会循环到另一端。
     *
     * 选项对应关系：
     *   第0项："赛博跑酷"   -> SCREEN_GAME
     *   第1项："经典 2048"  -> SCREEN_GAME_2048
     *   第2项："像素鸟"     -> SCREEN_GAME_FLAPPY
     *   第3项："八分音符酱" -> SCREEN_GAME_NOTE
     */
    lv_roller_set_options(game_roller,
                        "赛博跑酷\n"
                        "经典 2048\n"
                        "像素鸟\n"
                        "八分音符酱\n"
                        "俄罗斯方块",
                        LV_ROLLER_MODE_NORMAL);

    /**
     * 设置滚轮可见行数为2行。
     * 这意味着在屏幕上同时可以看到当前选中项以及相邻的一个选项，
     * 给用户一个上下文的参考，暗示列表可以滚动。
     */
    lv_roller_set_visible_row_count(game_roller, 2);

    /**
     * 将滚轮控件居中放置在屏幕上（相对于父对象的中心点对齐）。
     */
    lv_obj_center(game_roller);

    /**
     * 设置滚轮控件的宽度为200像素。
     * 高度会根据可见行数和字体大小自动计算，无需手动设置。
     * 200像素的宽度在常见的小屏幕设备上能提供良好的显示效果。
     */
    lv_obj_set_width(game_roller, 200);

    /**
     * 设置滚轮使用自定义中文字体（16像素），以正确显示中文游戏名称。
     */
    lv_obj_set_style_text_font(game_roller, &my_font_cn_16, 0);

    /* ---- 第四步：配置滚轮的详细样式 ---- */

    /**
     * 设置滚轮的背景颜色为黑色，与屏幕背景保持一致。
     * 参数0表示设置默认样式（主部件的默认状态）。
     */
    lv_obj_set_style_bg_color(game_roller, lv_color_black(), 0);

    /**
     * 设置滚轮边框宽度为0，移除默认边框，使界面更加简洁清爽。
     */
    lv_obj_set_style_border_width(game_roller, 0, 0);

    /**
     * 设置未选中项的文字颜色为灰色（0x888888）。
     * 灰色文字表示这些选项当前未被选中，视觉上不那么突出。
     */
    lv_obj_set_style_text_color(game_roller, lv_color_hex(0x888888), 0);

    /**
     * 设置选中项（LV_PART_SELECTED）的背景颜色为白色。
     * LV_PART_SELECTED 是LVGL的样式部件标志，表示滚轮中当前被选中的那一行。
     * 白色背景配合黑色文字形成高对比度，清晰标识当前选中项。
     */
    lv_obj_set_style_bg_color(game_roller, lv_color_white(), LV_PART_SELECTED);

    /**
     * 设置选中项的文字颜色为黑色。
     * 黑色文字与白色背景搭配，确保选中项的文字清晰可读。
     */
    lv_obj_set_style_text_color(game_roller, lv_color_black(), LV_PART_SELECTED);
}

/**
 * @brief 处理游戏列表界面接收到的用户命令
 *
 * 该函数是游戏列表界面的命令处理回调，由UI管理器在检测到用户手势后调用。
 * 根据不同的命令类型执行相应的操作：
 *   - UI_CMD_UP:    向上滚动滚轮选择上一个游戏
 *   - UI_CMD_DOWN:  向下滚动滚轮选择下一个游戏
 *   - UI_CMD_LEFT:  返回主菜单界面
 *   - UI_CMD_RIGHT: 确认选择，进入当前选中的游戏
 *
 * @param cmd 用户输入命令，类型为ui_cmd_t枚举，定义在ui_manager.h中
 *            可能的值包括：UI_CMD_UP、UI_CMD_DOWN、UI_CMD_LEFT、UI_CMD_RIGHT
 */
void game_list_screen_handle_cmd(ui_cmd_t cmd) {

    /* ---- 处理"向上"命令：滚动到上一个游戏选项 ---- */
    if (cmd == UI_CMD_UP) {
        /**
         * 获取滚轮当前选中的索引值（从0开始）。
         * 例如：选中"赛博跑酷"时返回0，选中"经典2048"时返回1。
         */
        uint16_t current_idx = lv_roller_get_selected(game_roller);

        /**
         * 如果当前索引大于0（不是第一项），则将选中项向上移动一位。
         * LV_ANIM_ON 表示启用滚动动画，使切换过程平滑流畅。
         * 当已在第一项时（current_idx == 0），不做任何操作，防止越界。
         */
        if (current_idx > 0) lv_roller_set_selected(game_roller, current_idx - 1, LV_ANIM_ON);
    }

    /* ---- 处理"向下"命令：滚动到下一个游戏选项 ---- */
    else if (cmd == UI_CMD_DOWN) {
        /**
         * 获取滚轮当前选中的索引值。
         */
        uint16_t current_idx = lv_roller_get_selected(game_roller);

        /**
         * 如果当前索引小于最大索引值（GAME_ITEM_COUNT - 1 = 3，即不是最后一项），
         * 则将选中项向下移动一位。启用滚动动画。
         * 当已在最后一项时，不做任何操作，防止越界。
         */
        if (current_idx < GAME_ITEM_COUNT - 1) lv_roller_set_selected(game_roller, current_idx + 1, LV_ANIM_ON);
    }

    /* ---- 处理"向左"命令：返回主菜单界面 ---- */
    else if (cmd == UI_CMD_LEFT) {
        /**
         * 声明switch_to_screen()函数的外部原型。
         * 该函数定义在ui_manager.c中，用于执行屏幕之间的切换。
         * 使用extern声明是因为该函数不在本文件中定义，但在此处需要调用。
         * 参数为ui_screen_state_t枚举值，表示目标屏幕的状态标识。
         */
        extern void switch_to_screen(ui_screen_state_t target);

        /**
         * 切换到主菜单界面（SCREEN_MENU）。
         * 这是返回操作，用户按下左键回到上级菜单。
         */
        switch_to_screen(SCREEN_MENU);
    }

    /* ---- 处理"向右"命令：确认选择并进入对应游戏 ---- */
    else if (cmd == UI_CMD_RIGHT) {
        /**
         * 获取当前滚轮选中的游戏索引。
         * 根据该索引值判断用户选择了哪个游戏，并切换到对应的游戏界面。
         */
        uint16_t selected_idx = lv_roller_get_selected(game_roller);

        /**
         * 声明switch_to_screen()函数的外部原型。
         * 虽然在上方的LEFT分支中已经声明过，但由于作用域在不同的if-else分支中，
         * 此处需要再次声明（C语言中extern声明可以在同一作用域内重复，无副作用）。
         */
        extern void switch_to_screen(ui_screen_state_t target);

        /**
         * 根据选中的索引值切换到对应的游戏界面：
         *   索引0 -> SCREEN_GAME       赛博跑酷游戏界面
         *   索引1 -> SCREEN_GAME_2048  经典2048游戏界面
         *   索引2 -> SCREEN_GAME_FLAPPY 像素鸟游戏界面
         *   索引3 -> SCREEN_GAME_NOTE  八分音符酱游戏界面
         *
         * 使用if-else if链而非switch语句，因为分支较少，可读性更好。
         */
        if (selected_idx == 0) {
            switch_to_screen(SCREEN_GAME);
        }
        else if (selected_idx == 1) {
            switch_to_screen(SCREEN_GAME_2048);
        }
        else if (selected_idx == 2) {
            switch_to_screen(SCREEN_GAME_FLAPPY);
        }
        else if (selected_idx == 3) {
            switch_to_screen(SCREEN_GAME_NOTE);
        }
        else if (selected_idx == 4) {
            switch_to_screen(SCREEN_GAME_TETRIS);
        }
    }
}
