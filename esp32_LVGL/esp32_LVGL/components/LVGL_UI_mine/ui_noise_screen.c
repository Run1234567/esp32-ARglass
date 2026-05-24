#include "ui_noise_screen.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <stdio.h> 

lv_obj_t * ui_noise_screen;
static lv_obj_t * meter_obj;
static lv_meter_indicator_t * needle_indic;
static lv_obj_t * label_db; 
static lv_obj_t * label_status; // ? 新增：动态状态标签

void ui_noise_screen_init(void) {
    ui_noise_screen = lv_obj_create(NULL);
    
    // ? 1. 豪华渐变背景 (深蓝灰 -> 纯黑)
    lv_obj_set_style_bg_color(ui_noise_screen, lv_color_hex(0x14141C), 0);
    lv_obj_set_style_bg_grad_color(ui_noise_screen, lv_color_hex(0x050508), 0);
    lv_obj_set_style_bg_grad_dir(ui_noise_screen, LV_GRAD_DIR_VER, 0);

// ? 2. 顶部标题
    lv_obj_t * title = lv_label_create(ui_noise_screen);
    lv_label_set_text(title, "噪声监测"); // ? 改成中文
    // ? 极其关键：必须把这个标签的字体切换成你的中文字体！
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0); 
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF973B), 0); // 低调的高级灰
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // ? 3. 创建仪表盘主体
    meter_obj = lv_meter_create(ui_noise_screen);
    lv_obj_center(meter_obj);
    lv_obj_set_size(meter_obj, 220, 220); // 稍微放大一点
    // ? 核心技巧：去掉默认的难看背景和边框，使其悬浮融合
    lv_obj_set_style_bg_opa(meter_obj, 0, 0);
    lv_obj_set_style_border_opa(meter_obj, 0, 0);

    // ? 4. 汽车级刻度盘
    lv_meter_scale_t * scale = lv_meter_add_scale(meter_obj);
    // 细分刻度 (暗灰色，密集)
    lv_meter_set_scale_ticks(meter_obj, scale, 46, 2, 8, lv_color_hex(0x555566));
    // 主刻度 (亮白色，稍长，带数字)
    lv_meter_set_scale_major_ticks(meter_obj, scale, 9, 3, 14, lv_color_white(), 15);
    // 量程改为 30~120 (现实中安静环境也有 30+ dB，这样指针摆动更灵敏)
    lv_meter_set_scale_range(meter_obj, scale, 30, 120, 260, 140);

    // ? 5. 超粗霓虹发光环
    int arc_w = 12; // 加粗的光环宽度
    lv_meter_indicator_t * arc1 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0x00FFCC), 0); // 荧光青
    lv_meter_set_indicator_start_value(meter_obj, arc1, 30);
    lv_meter_set_indicator_end_value(meter_obj, arc1, 60);

    lv_meter_indicator_t * arc2 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0xFFCC00), 0); // 警示黄
    lv_meter_set_indicator_start_value(meter_obj, arc2, 60);
    lv_meter_set_indicator_end_value(meter_obj, arc2, 85);

    lv_meter_indicator_t * arc3 = lv_meter_add_arc(meter_obj, scale, arc_w, lv_color_hex(0xFF3333), 0); // 危险红
    lv_meter_set_indicator_start_value(meter_obj, arc3, 85);
    lv_meter_set_indicator_end_value(meter_obj, arc3, 120);

    // ? 6. 机械感红色指针
    needle_indic = lv_meter_add_needle_line(meter_obj, scale, 3, lv_color_hex(0xFF3333), -15);

    // ? 给指针加一个高端的中心轴承圆点
    lv_obj_t * center_dot = lv_obj_create(meter_obj);
    lv_obj_set_size(center_dot, 16, 16);
    lv_obj_align(center_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_dot, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(center_dot, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_border_width(center_dot, 2, 0);

    // ? 7. 居中大数字 dB 标签
    label_db = lv_label_create(ui_noise_screen);
    lv_obj_align(label_db, LV_ALIGN_CENTER, 0, 65); // 放在轴承正下方
    lv_obj_set_style_text_color(label_db, lv_color_white(), 0);
    lv_label_set_text(label_db, "-- dB");

    // ? 8. 底部状态指示器
    label_status = lv_label_create(ui_noise_screen);
    lv_obj_set_style_text_font(label_status, &my_font_cn_16, 0);
    lv_obj_align(label_status, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0x8888AA), 0);
    lv_label_set_text(label_status, "等待数据...");
}

// 供串口调用的更新函数
void update_noise_meter(int val) {
    if (lvgl_port_lock(0)) {
        // 限制在新的量程 30-120 范围内
        if (val < 30) val = 30;
        if (val > 120) val = 120;
        
        // 更新机械指针
        lv_meter_set_indicator_value(meter_obj, needle_indic, val);
        
        // 更新中心数字
        if (label_db != NULL) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d dB", val);
            lv_label_set_text(label_db, buf);
        }

        // ✨ 互动灵魂：根据分贝值，字体颜色和状态提示动态改变！
        if (label_status != NULL && label_db != NULL) {
            if (val < 60) {
                // 安全环境：青色
                lv_obj_set_style_text_color(label_db, lv_color_hex(0x00FFCC), 0);
                lv_label_set_text(label_status, "安全 (安静)"); // ✨ 改为中文
                lv_obj_set_style_text_color(label_status, lv_color_hex(0x00FFCC), 0);
            } else if (val < 85) {
                // 中等噪音：黄色
                lv_obj_set_style_text_color(label_db, lv_color_hex(0xFFCC00), 0);
                lv_label_set_text(label_status, "中等噪音"); // ✨ 改为中文
                lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFCC00), 0);
            } else {
                // 危险噪音：红色
                lv_obj_set_style_text_color(label_db, lv_color_hex(0xFF3333), 0);
                lv_label_set_text(label_status, "危险 (嘈杂)"); // ✨ 改为中文
                lv_obj_set_style_text_color(label_status, lv_color_hex(0xFF3333), 0);
            }
        }
        
        lvgl_port_unlock();
    }
}