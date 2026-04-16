#include "ui_menu_screen.h"
#include "ui_globals.h" // 引入全局变量枢纽

// =========================================================
// 🌍 真正定义菜单界面的全局对象 (分配内存)
// =========================================================
lv_obj_t * ui_menu_screen;
lv_obj_t * menu_roller;

// =========================================================
// 🚀 菜单界面初始化函数
// =========================================================
void ui_menu_screen_init(void) {
    // 1. 创建全新的菜单屏幕
    ui_menu_screen = lv_obj_create(NULL);
    
    // 设置黑底（AR眼镜必备）
    lv_obj_set_style_bg_color(ui_menu_screen, lv_color_black(), 0); 
    lv_obj_set_style_bg_opa(ui_menu_screen, LV_OPA_COVER, 0);

    // 2. 顶部标题
    lv_obj_t * label_title = lv_label_create(ui_menu_screen);
    lv_obj_set_style_text_font(label_title, &my_font_cn_16, 0); 
    lv_obj_set_style_text_color(label_title, lv_color_hex(0x00FF00), 0); // 科技绿标题
    lv_label_set_text(label_title, "主菜单");
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 20);

    // 3. 创建核心的滚轮部件 (Roller)
    menu_roller = lv_roller_create(ui_menu_screen);
    
    // 设置滚轮的选项（每一行代表一个菜单项，带 Emoji 图标提升视觉效果）
    lv_roller_set_options(menu_roller,
                        "🏠 回到主页\n"
                        "❤️ 健康监测\n"
                        "🤖 AI 对话\n"
                        "⚙️ 系统设置",
                        LV_ROLLER_MODE_INFINITE); // 无限循环模式，滚到底会自动接上开头

    // 4. 设置滚轮的排版与尺寸
    lv_roller_set_visible_row_count(menu_roller, 3); // 屏幕上同时显示 3 行选项
    lv_obj_center(menu_roller);                      // 放在屏幕正中间
    lv_obj_set_width(menu_roller, 200);              // 限制滚轮宽度

    // 5. 设置滚轮的字体与颜色样式
    lv_obj_set_style_text_font(menu_roller, &my_font_cn_16, 0); 
    
    // 未选中项的样式：纯黑背景，暗灰色文字（不抢眼）
    lv_obj_set_style_bg_color(menu_roller, lv_color_black(), 0);
    lv_obj_set_style_border_width(menu_roller, 0, 0); // 去除自带的边框
    lv_obj_set_style_text_color(menu_roller, lv_color_hex(0x888888), 0);

    // 被选中项的样式：高亮白色背景，黑色文字（焦点非常清晰）
    lv_obj_set_style_bg_color(menu_roller, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_text_color(menu_roller, lv_color_black(), LV_PART_SELECTED);
}