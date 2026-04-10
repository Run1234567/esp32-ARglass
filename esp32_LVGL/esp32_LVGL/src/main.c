#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tft_display.h"

// 引入 LVGL 核心与移植包
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "MAIN";

// ==========================================
// ? LVGL 动画回调函数：在动画刷新时更新圆弧和文字
// ==========================================
static void arc_anim_cb(void * var, int32_t v) {
    lv_obj_t * arc = (lv_obj_t *)var;
    lv_arc_set_value(arc, v); // 更新圆弧的进度值
    
    // 获取圆弧内部的子对象（也就是我们创建的文本标签），并更新显示的数字
    lv_obj_t * label = lv_obj_get_child(arc, 0); 
    if (label) {
        lv_label_set_text_fmt(label, "%d %%", (int)v);
    }
}

// ==========================================
// ? 核心 UI 构建函数
// ==========================================
void create_fancy_ui(void) {
    // 1. 将屏幕背景设为深邃的藏青色 (赛博朋克暗色调)
    lv_obj_t * scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x2a2a4c), 0);

    // 2. 创建一个圆弧对象 (Arc)
    lv_obj_t * arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 180, 180);       // 占据大部分屏幕
    lv_obj_center(arc);                   // 绝对居中
    lv_arc_set_rotation(arc, 270);        // 起点旋转到顶部
    lv_arc_set_bg_angles(arc, 0, 360);    // 背景画一个完整的圆
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB); // 去掉拖拽用的“小手柄”，我们只做展示

    // --- 开始疯狂堆叠特效 ---
    // 设置圆弧背景轨道的颜色和宽度 (暗蓝色)
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x14143c), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_MAIN);
    
    // 设置进度条的颜色和宽度 (青蓝色)
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x00f0ff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    
    // ? 最华丽的一步：给进度条加上发光外发光阴影！
    lv_obj_set_style_shadow_color(arc, lv_color_hex(0x00f0ff), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(arc, 25, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_spread(arc, 5, LV_PART_INDICATOR);

    // 3. 在圆弧正中间创建一个百分比文本标签
    lv_obj_t * label = lv_label_create(arc); // 注意：父对象是 arc
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_white(), 0); // 纯白文字

    // 4. 创建一个 LVGL 动画 (Animation)，让圆弧自己动起来
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);                  // 绑定动画对象：我们的圆弧
    lv_anim_set_exec_cb(&a, arc_anim_cb);      // 绑定刚才写的动画回调函数
    lv_anim_set_time(&a, 2000);                // 动画时长 2 秒
    lv_anim_set_playback_time(&a, 1000);       // 退回时长 1 秒 (达到100%后倒退回0%)
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE); // 无限循环！
    lv_anim_set_values(&a, 0, 100);            // 变量范围：从 0 变到 100
    lv_anim_start(&a);                         // 开机！
}

void app_main(void) {
    ESP_LOGI(TAG, "1. 启动物理屏幕驱动...");
    lcd_init();

    ESP_LOGI(TAG, "2. 初始化 LVGL 移植层...");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG, "3. 将屏幕挂载到 LVGL...");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = 240 * 240 / 10, 
        .double_buffer = true,
        .hres = 240,
        .vres = 240,
        .monochrome = false,
        .flags = { .buff_dma = true }
    };
    lvgl_port_add_disp(&disp_cfg);

    ESP_LOGI(TAG, "4. 绘制华丽的 UI...");
    create_fancy_ui(); // 调用我们刚才写的 UI 函数

    ESP_LOGI(TAG, "主任务进入休眠...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}