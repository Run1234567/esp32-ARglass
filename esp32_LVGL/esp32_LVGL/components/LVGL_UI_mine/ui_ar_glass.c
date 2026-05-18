#include "ui_ar_glass.h"
#include "ui_globals.h" // 引入全局变量

#include <time.h>
#include "esp_lvgl_port.h"



// 引入你的中文字库
LV_FONT_DECLARE(my_font_cn_16);

// =========================================================
// ? 全局变量定义 
// =========================================================
// ? 新增：定义一个代表“主页面”的全局对象，方便以后来回切换
lv_obj_t * ui_main_screen;  

lv_obj_t * label_time;      // 时间标签
lv_obj_t * label_date;      // 阳历日期标签
lv_obj_t * label_lunar;     // 农历标签
lv_obj_t * label_weather;   // 天气文本标签
lv_obj_t * label_batt_pct;  // 电量数字标签
lv_obj_t * icon_batt;       // 电池图标标签

// =========================================================
// ? 主界面初始化函数
// =========================================================
void ui_ar_glass_init(void) {
    // ? 修改 1：不要再用 lv_scr_act() 了！
    // 创建一个全新的、干净的后台屏幕对象
    ui_main_screen = lv_obj_create(NULL); 
    
    // 给这个新屏幕设置黑底
    lv_obj_set_style_bg_color(ui_main_screen, lv_color_black(), 0); 
    lv_obj_set_style_bg_opa(ui_main_screen, LV_OPA_COVER, 0);

    // 2. 创建通用样式
    static lv_style_t style_common;
    lv_style_init(&style_common);
    lv_style_set_text_color(&style_common, lv_color_white());
    lv_style_set_text_font(&style_common, &my_font_cn_16);

    // -----------------------------------------------------
    // A. 上方：农历信息
    // ? 修改 2：所有的父对象都要改成 ui_main_screen (后面同理)
    // -----------------------------------------------------
    label_lunar = lv_label_create(ui_main_screen); 
    lv_obj_add_style(label_lunar, &style_common, 0);
    lv_label_set_text(label_lunar, "加载中..."); 
    lv_obj_align(label_lunar, LV_ALIGN_TOP_MID, 0, 40);

    // -----------------------------------------------------
    // B. 中间：大时间
    // -----------------------------------------------------
    label_time = lv_label_create(ui_main_screen);
    lv_obj_set_style_text_font(label_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(label_time, lv_color_hex(0x00FF00), 0); 
    lv_label_set_text(label_time, "00:00");
    lv_obj_align(label_time, LV_ALIGN_CENTER, 0, 0);

    // -----------------------------------------------------
    // C. 下方：公历日期
    // -----------------------------------------------------
    label_date = lv_label_create(ui_main_screen);
    lv_obj_add_style(label_date, &style_common, 0); 
    lv_label_set_text(label_date, "00/00");
    lv_obj_align(label_date, LV_ALIGN_CENTER, 0, 45);

    // -----------------------------------------------------
    // D. 底部：天气容器与图标
    // -----------------------------------------------------
    lv_obj_t * weather_cont = lv_obj_create(ui_main_screen);
    lv_obj_set_size(weather_cont, 200, 50);
    lv_obj_set_style_bg_opa(weather_cont, 0, 0);      
    lv_obj_set_style_border_opa(weather_cont, 0, 0);  
    lv_obj_align(weather_cont, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(weather_cont, LV_FLEX_FLOW_ROW); 
    lv_obj_set_flex_align(weather_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * icon_weather = lv_label_create(weather_cont);
    lv_label_set_text(icon_weather, LV_SYMBOL_IMAGE); 
    lv_obj_set_style_text_color(icon_weather, lv_palette_main(LV_PALETTE_YELLOW), 0);

    label_weather = lv_label_create(weather_cont);
    lv_obj_add_style(label_weather, &style_common, 0); 
    lv_label_set_text(label_weather, " 获取中...");

    // -----------------------------------------------------
    // E. 右上角：电量显示
    // -----------------------------------------------------
    lv_obj_t * battery_cont = lv_obj_create(ui_main_screen);
    lv_obj_set_size(battery_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(battery_cont, 0, 0);
    lv_obj_set_style_border_opa(battery_cont, 0, 0);
    lv_obj_set_style_pad_all(battery_cont, 0, 0);
    lv_obj_align(battery_cont, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_flex_flow(battery_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_cont, 5, 0);

    label_batt_pct = lv_label_create(battery_cont);
    lv_obj_add_style(label_batt_pct, &style_common, 0);
    lv_label_set_text(label_batt_pct, "---%");

    icon_batt = lv_label_create(battery_cont);
    lv_obj_set_style_text_color(icon_batt, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(icon_batt, LV_SYMBOL_BATTERY_FULL); 
}

// =========================================================
// ⏱️ 时间与 UI 刷新守护任务
// =========================================================
void ui_time_update_task(void *pvParameters) {
    while (1) {
        time_t now;
        struct tm timeinfo;
        
        // 获取当前系统时间
        time(&now);
        localtime_r(&now, &timeinfo);

        // 判断时间是否已经同步成功 (年份大于 1970 代表同步成功)
        if (timeinfo.tm_year > (2020 - 1900)) {
            char time_str[16];
            char date_str[16];
            
            // 格式化时间为 "HH:MM" (例如 14:30)
            strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
            // 格式化日期为 "MM/DD" (例如 05/20)
            strftime(date_str, sizeof(date_str), "%m/%d", &timeinfo);

            // ⚠️ 极其重要：操作 LVGL 必须加锁！
            if (lvgl_port_lock(0)) {
                lv_label_set_text(label_time, time_str);
                lv_label_set_text(label_date, date_str);
                lvgl_port_unlock();
            }
        }
        
        // 每 1 秒钟刷新一次就够了，非常省电
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
