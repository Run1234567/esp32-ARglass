/**
 * @file ui_noise_screen.c
 * @brief 噪声监测屏幕 UI 实现
 *
 * 本文件实现了一个基于 LVGL 仪表盘的噪声监测界面，用于实时显示环境噪声分贝值。
 * 界面包含：渐变背景、中文标题、刻度仪表盘、彩色弧形区域、机械指针、
 * 分贝数值标签、以及根据噪声等级动态变化的状态文字。
 *
 * 设计风格：汽车仪表盘 + 霓虹发光效果，量程 30~120 dB。
 * 颜色分级：
 *   - 30~60 dB  荧光绿  安静环境
 *   - 60~85 dB  警示黄  中等噪音
 *   - 85~120 dB 危险红  嘈杂环境
 */

#include "ui_globals.h"       /* 全局变量与字体声明（包含 my_font_cn_16 中文字体） */
#include "ui_noise_screen.h"  /* 噪声屏幕的头文件，声明 ui_noise_screen 对象及初始化/更新接口 */
#include "lvgl.h"             /* LVGL 图形库核心头文件 */
#include "esp_lvgl_port.h"    /* ESP-IDF 的 LVGL 移植层，提供 lvgl_port_lock/unlock 线程安全接口 */
#include <stdio.h>            /* 标准 I/O 库，用于 snprintf 格式化分贝数值字符串 */

/*============================================================================
 *  全局 / 静态变量
 *============================================================================*/

/* 噪声屏幕的根对象（全局），供 ui_manager 通过 lv_scr_load 切换页面时使用 */
lv_obj_t * ui_noise_screen;

/* 仪表盘对象指针（静态），在 init 中创建，在 update 中用于更新指针数值 */
static lv_obj_t * meter_obj;

/* 仪表盘指针指示器（静态），记录红色指针的句柄，供 update 中动态改变其指向值 */
static lv_meter_indicator_t * needle_indic;

/* 分贝数值标签（静态），显示在仪表盘中央下方，内容如 "75 dB" */
static lv_obj_t * label_db;

/* 状态文字标签（静态），显示在屏幕底部，根据分贝等级动态切换提示文字 */
static lv_obj_t * label_status;

/*============================================================================
 *  ui_noise_screen_init  --  噪声屏幕初始化函数
 *
 *  功能：创建并布局噪声监测页面的所有 UI 元素。
 *  调用时机：系统启动时由 ui_manager 调用一次，完成页面构造。
 *  执行流程：
 *    1. 创建页面根对象并设置渐变背景
 *    2. 创建顶部中文标题
 *    3. 创建仪表盘主体并配置刻度
 *    4. 添加三段彩色弧形区域（绿/黄/红）
 *    5. 添加红色机械指针与中心轴承装饰
 *    6. 创建分贝数值标签
 *    7. 创建底部状态提示标签
 *============================================================================*/
void ui_noise_screen_init(void) {

    /*------------------------------------------------------------------
     * 第一步：创建页面根对象
     *
     * lv_obj_create(NULL) 创建一个全新的屏幕对象，不挂载到任何父对象。
     * 该对象作为整个噪声页面的根容器，后续所有子控件都创建在它上面。
     *------------------------------------------------------------------*/
    ui_noise_screen = lv_obj_create(NULL);

    /*------------------------------------------------------------------
     * 第二步：设置豪华渐变背景（深蓝色 -> 纯黑色）
     *
     * bg_color     : 渐变起始颜色（顶部），深蓝黑 0x14141C
     * bg_grad_color: 渐变终止颜色（底部），纯黑 0x050508
     * bg_grad_dir  : 渐变方向为垂直（从上到下）
     *------------------------------------------------------------------*/
    lv_obj_set_style_bg_color(ui_noise_screen, lv_color_hex(0x14141C), 0);
    lv_obj_set_style_bg_grad_color(ui_noise_screen, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_grad_dir(ui_noise_screen, LV_GRAD_DIR_VER, 0);

    /*------------------------------------------------------------------
     * 第三步：创建顶部标题标签
     *
     * 在屏幕顶部居中位置显示"噪声监测"四个字。
     * 必须使用自定义中文字体 my_font_cn_16，否则中文会显示为乱码或方块。
     * 文字颜色为橙色 0xFF973B，偏暖色调，与深色背景形成对比。
     *------------------------------------------------------------------*/
    lv_obj_t * title = lv_label_create(ui_noise_screen);
    lv_label_set_text(title, "噪声监测");
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);   /* 切换为中文字体，关键步骤 */
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF973B), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);            /* 对齐到屏幕顶部居中，Y 偏移 5px */

    /*------------------------------------------------------------------
     * 第四步：创建仪表盘主体
     *
     * lv_meter_create 创建一个 LVGL 仪表盘控件。
     * 尺寸 220x220 像素，居中放置在屏幕上。
     * 背景和边框透明度设为 0，使其与页面背景融合，呈现悬浮效果。
     *------------------------------------------------------------------*/
    meter_obj = lv_meter_create(ui_noise_screen);
    lv_obj_center(meter_obj);                                /* 居中放置 */
    lv_obj_set_size(meter_obj, 220, 220);                    /* 仪表盘尺寸 */
    lv_obj_set_style_bg_opa(meter_obj, 0, 0);               /* 背景完全透明 */
    lv_obj_set_style_border_opa(meter_obj, 0, 0);           /* 边框完全透明 */

    /*------------------------------------------------------------------
     * 第五步：配置刻度盘
     *
     * 先添加一个刻度（scale），然后分别设置细刻度和主刻度：
     *   - 细刻度（minor ticks）：46 根，宽度 2px，长度 8px，暗灰色
     *   - 主刻度（major ticks）：每 9 根细刻度出现 1 根主刻度，
     *     宽度 3px，长度 14px，白色，偏移 15px 显示数字
     *   - 量程范围：30 ~ 120 dB，角度跨度 260 度，起始角度 140 度
     *------------------------------------------------------------------*/
    lv_meter_scale_t * scale = lv_meter_add_scale(meter_obj);

    /* 细刻度：46 根短线段，宽度 2，长度 8，暗灰色 0x555566 */
    lv_meter_set_scale_ticks(meter_obj, scale, 46, 2, 8, lv_color_hex(0x555566));

    /* 主刻度：每 9 根细刻度出现一根，宽度 3，长度 14，白色，数字偏移 15px */
    lv_meter_set_scale_major_ticks(meter_obj, scale, 9, 3, 14, lv_color_white(), 15);

    /* 量程设置：最小值 30，最大值 120，角度跨度 260 度，起始角度 140 度 */
    lv_meter_set_scale_range(meter_obj, scale, 30, 120, 260, 140);

    /*------------------------------------------------------------------
     * 第六步：添加三段霓虹发光弧形区域
     *
     * 将仪表盘外圈分为三段彩色弧线，直观表示噪声等级：
     *   - 绿色弧 (0x00FFCC 荧光绿)：30 ~ 60 dB  安静环境
     *   - 黄色弧 (0xFFCC00 警示黄)：60 ~ 85 dB  中等噪音
     *   - 红色弧 (0xFF3333 危险红)：85 ~ 120 dB 嘈杂环境
     * arc_w 为弧线宽度，值越大视觉效果越粗。
     *------------------------------------------------------------------*/
    int arc_w = 12;  /* 弧线宽度（像素） */

    /* 荧光绿色弧：30 ~ 60 dB，代表安静环境 */
    lv_meter_indicator_t * arc1 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0x00FFCC), 0);
    lv_meter_set_indicator_start_value(meter_obj, arc1, 30);
    lv_meter_set_indicator_end_value(meter_obj, arc1, 60);

    /* 警示黄色弧：60 ~ 85 dB，代表中等噪音 */
    lv_meter_indicator_t * arc2 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0xFFCC00), 0);
    lv_meter_set_indicator_start_value(meter_obj, arc2, 60);
    lv_meter_set_indicator_end_value(meter_obj, arc2, 85);

    /* 危险红色弧：85 ~ 120 dB，代表嘈杂环境 */
    lv_meter_indicator_t * arc3 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0xFF3333), 0);
    lv_meter_set_indicator_start_value(meter_obj, arc3, 85);
    lv_meter_set_indicator_end_value(meter_obj, arc3, 120);

    /*------------------------------------------------------------------
     * 第七步：添加机械感红色指针
     *
     * needle_line 类型的指示器：线宽 3px，红色 0xFF3333，尾部延伸 -15px。
     * 尾部负值表示指针会向圆心方向延伸一小段，模拟真实仪表指针的铆钉效果。
     * needle_indic 保存为静态变量，供 update_noise_meter() 动态更新指向值。
     *------------------------------------------------------------------*/
    needle_indic = lv_meter_add_needle_line(meter_obj, scale, 3, lv_color_hex(0xFF3333), -15);

    /*------------------------------------------------------------------
     * 第八步：指针中心轴承装饰圆点
     *
     * 在仪表盘正中心创建一个 16x16 的圆形小部件，模拟机械轴承外观：
     *   - 深黑色背景 0x111111
     *   - 红色边框 0xFF3333，宽度 2px
     *   - 圆角设为 LV_RADIUS_CIRCLE 使其呈现完美圆形
     *------------------------------------------------------------------*/
    lv_obj_t * center_dot = lv_obj_create(meter_obj);
    lv_obj_set_size(center_dot, 16, 16);
    lv_obj_align(center_dot, LV_ALIGN_CENTER, 0, 0);       /* 正中心对齐 */
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(center_dot, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_border_width(center_dot, 2, 0);

    /*------------------------------------------------------------------
     * 第九步：居中分贝数值标签
     *
     * 显示当前分贝值，如 "75 dB"。
     * 放置在轴承圆点正下方（Y 偏移 65px），白色文字。
     * 初始文本 "-- dB" 表示尚未接收到数据。
     *------------------------------------------------------------------*/
    label_db = lv_label_create(ui_noise_screen);
    lv_obj_align(label_db, LV_ALIGN_CENTER, 0, 65);        /* 位于轴承下方 */
    lv_obj_set_style_text_color(label_db, lv_color_white(), 0);
    lv_label_set_text(label_db, "-- dB");                   /* 初始占位文本 */

    /*------------------------------------------------------------------
     * 第十步：底部状态提示标签
     *
     * 根据当前噪声等级动态显示中文提示：
     *   - "安全 (安静)"   荧光绿
     *   - "中等噪音"      警示黄
     *   - "危险 (嘈杂)"   危险红
     *   - "等待数据..."    初始状态，灰色
     * 使用中文字体 my_font_cn_16。
     *------------------------------------------------------------------*/
    label_status = lv_label_create(ui_noise_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);  /* 中文字体 */
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -20);     /* 屏幕底部居中，上移 20px */
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x8888AA), 0);  /* 淡灰紫色 */
    lv_label_set_text(label_status, "等待数据...");               /* 初始提示文字 */
}

/*============================================================================
 *  update_noise_meter  --  更新噪声仪表盘数值
 *
 *  @param val  当前分贝值（整数，单位 dB）
 *
 *  功能：由串口接收任务或传感器采集任务调用，将最新的分贝值
 *        同步更新到仪表盘指针、分贝数字标签和状态提示标签。
 *
 *  线程安全：使用 lvgl_port_lock(0) 获取 LVGL 互斥锁，
 *            确保在多任务环境下对 LVGL 对象的操作不会产生竞态条件。
 *            锁的超时时间为 0（立即获取，失败则跳过）。
 *
 *  执行流程：
 *    1. 获取 LVGL 互斥锁
 *    2. 将输入值钳位到量程范围 [30, 120]
 *    3. 更新指针指向值
 *    4. 更新分贝数字标签文本
 *    5. 根据分贝等级设置数字和状态标签的颜色与文字
 *    6. 释放互斥锁
 *============================================================================*/
void update_noise_meter(int val) {

    /* 尝试获取 LVGL 互斥锁，成功后才能安全操作 UI 对象 */
    if (lvgl_port_lock(0)) {

        /*------------------------------------------------------------------
         * 数值钳位：确保分贝值在仪表盘量程 [30, 120] 范围内
         * 低于 30 dB 按 30 处理，高于 120 dB 按 120 处理
         *------------------------------------------------------------------*/
        if (val < 30) val = 30;
        if (val > 120) val = 120;

        /*------------------------------------------------------------------
         * 更新仪表盘红色指针的指向值
         * lv_meter_set_indicator_value 会驱动指针平滑移动到新的刻度位置
         *------------------------------------------------------------------*/
        lv_meter_set_indicator_value(meter_obj, needle_indic, val);

        /*------------------------------------------------------------------
         * 更新分贝数字标签
         * 将整数分贝值格式化为字符串（如 "75 dB"）并设置到标签控件
         * 先检查 label_db 是否为 NULL，防止初始化未完成时的空指针访问
         *------------------------------------------------------------------*/
        if (label_db != NULL) {
            char buf[16];                              /* 格式化缓冲区，足够存放 "120 dB" + 结尾符 */
            snprintf(buf, sizeof(buf), "%d dB", val);  /* 格式化为 "数值 dB" 形式 */
            lv_label_set_text(label_db, buf);          /* 更新标签文本 */
        }

        /*------------------------------------------------------------------
         * 根据分贝等级动态改变数字标签和状态标签的颜色与文字
         *
         * 三个等级：
         *   1. val < 60  安静环境   荧光绿 0x00FFCC  文字"安全 (安静)"
         *   2. val < 85  中等噪音   警示黄 0xFFCC00  文字"中等噪音"
         *   3. val >= 85 嘈杂环境   危险红 0xFF3333  文字"危险 (嘈杂)"
         *
         * 同时检查两个标签指针非空，避免未初始化时崩溃
         *------------------------------------------------------------------*/
        if (label_status != NULL && label_db != NULL) {

            if (val < 60) {
                /*--- 安静环境（30~60 dB）：荧光绿色主题 ---*/
                lv_obj_set_style_text_color(label_db, lv_color_hex(0x00FFCC), 0);     /* 分贝数字变绿 */
                lv_label_set_text(label_status, "安全 (安静)");                        /* 状态文字 */
                lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FFCC), 0);  /* 状态文字变绿 */

            } else if (val < 85) {
                /*--- 中等噪音（60~85 dB）：警示黄色主题 ---*/
                lv_obj_set_style_text_color(label_db, lv_color_hex(0xFFCC00), 0);     /* 分贝数字变黄 */
                lv_label_set_text(label_status, "中等噪音");                           /* 状态文字 */
                lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFCC00), 0);  /* 状态文字变黄 */

            } else {
                /*--- 嘈杂环境（85~120 dB）：危险红色主题 ---*/
                lv_obj_set_style_text_color(label_db, lv_color_hex(0xFF3333), 0);     /* 分贝数字变红 */
                lv_label_set_text(label_status, "危险 (嘈杂)");                        /* 状态文字 */
                lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF3333), 0);  /* 状态文字变红 */
            }
        }

        /* 释放 LVGL 互斥锁，允许其他任务访问 UI */
        lvgl_port_unlock();
    }
}
