#include "ui_clock_screen.h"
#include "ui_globals.h"
#include <stdio.h>
#include <time.h>        // ? 新增：为了获取系统时间 
#include <sys/time.h>    // ? 新增：为了获取系统时间 

// ? 定义世界时间结构体 
typedef struct { 
    const char * name; 
    int offset; // 相对 UTC 的偏移小时数 
} city_time_t; 
 
// 精选 12 个著名城市/时区 
static const city_time_t world_cities[] = { 
    {"?? 北京 (CST)", 8}, 
    {"?? 东京 (JST)", 9}, 
    {"?? 悉尼 (AEST)", 10}, 
    {"?? 奥克兰 (NZDT)", 12}, 
    {"?? 迪拜 (GST)", 4}, 
    {"?? 莫斯科 (MSK)", 3}, 
    {"?? 巴黎 (CET)", 1}, 
    {"?? 伦敦 (GMT)", 0}, 
    {"?? 里约热内卢", -3}, 
    {"?? 纽约 (EST)", -5}, 
    {"?? 洛杉矶 (PST)", -8}, 
    {"?? 夏威夷 (HST)", -10} 
}; 
 
// 宏定义：计算数组长度 
#define WORLD_CITY_COUNT (sizeof(world_cities) / sizeof(world_cities[0])) 
static uint8_t world_city_idx = 0; // 默认索引 0 (北京) 

lv_obj_t * ui_clock_screen;
static lv_obj_t * label_clock_title;
static lv_obj_t * label_clock_content; // 统一用一个 Label 显示数字和中文提示

// 0=闹钟, 1=秒表, 2=世界时间, 3=倒计时
static uint8_t sub_mode = 0; 
// 0: 浏览层, 1: 详情/编辑层
static uint8_t view_level = 0; 
// 0: 未编辑, 1: 编辑分钟, 2: 编辑秒数
static uint8_t edit_state = 0; 
static bool is_running = false;

// ? 新增：闹钟的核心变量 
static uint8_t alarm_h = 8;          // 默认早上 8 点 
static uint8_t alarm_m = 0;          // 默认 0 分 
static bool alarm_enabled = false;   // 默认关闭 
static bool alarm_is_ringing = false;// 当前是否正在响铃！ 

// 核心时间变量
static uint32_t stopwatch_ms = 0;
static uint32_t countdown_ms = 3 * 60 * 1000; 
static lv_timer_t * timer_handle = NULL;

// ==========================================
// ? 刷新 UI 显示 (全中文 + 颜色高亮)
// ==========================================
static void update_clock_display(void) {
    char buf[128];
    
    // ?? 极其关键：必须开启此选项，LVGL 才会解析 #FFFF00 这种颜色代码！
    lv_label_set_recolor(label_clock_content, true);

    // ------------------------------------
    // 【层级 0】浏览预览界面
    // ------------------------------------
    if (view_level == 0) {
        lv_obj_set_style_text_color(label_clock_content, lv_color_hex(0x888888), 0); 
        switch (sub_mode) {
            // ? 修改：闹钟预览能直接看到设定的时间和状态 
            case 0: 
                lv_label_set_text(label_clock_title, "? 闹钟"); 
                sprintf(buf, "%02d:%02d (%s)\n右滑进入", alarm_h, alarm_m, alarm_enabled ? "开" : "关"); 
                break; 
            case 1: lv_label_set_text(label_clock_title, "?? 秒表"); sprintf(buf, "00:00.0\n右滑进入"); break;
            // ? 修改：世界时间预览界面显示当前选中的城市 
            case 2: 
                lv_label_set_text(label_clock_title, "? 世界时间"); 
                sprintf(buf, "%s\n右滑查看", world_cities[world_city_idx].name); 
                break; 
            case 3: lv_label_set_text(label_clock_title, "? 倒计时"); sprintf(buf, "03:00.0\n右滑进入"); break;
        }
        lv_label_set_text(label_clock_content, buf);
        return;
    }

    // ------------------------------------
    // 【层级 1】真实操作界面
    // ------------------------------------
    lv_obj_set_style_text_color(label_clock_content, lv_color_white(), 0); 

    if (sub_mode == 0) { 
        // ?【闹钟界面】 
        if (alarm_is_ringing) { 
            // 响铃时的暴走状态：全红！ 
            sprintf(buf, "#FF0000 %02d:%02d#\n#FF0000 ? 起床啦！#\n左滑/右滑 关闭", alarm_h, alarm_m); 
        } else { 
            // 正常编辑与查看状态 
            if (edit_state == 1) { 
                sprintf(buf, "#FFFF00 %02d#:%02d\n上下调整 [小时]", alarm_h, alarm_m); 
            } else if (edit_state == 2) { 
                sprintf(buf, "%02d:#FFFF00 %02d#\n上下调整 [分钟]", alarm_h, alarm_m); 
            } else if (edit_state == 3) { 
                // 焦点在开关上 
                sprintf(buf, "%02d:%02d\n状态: %s", alarm_h, alarm_m, alarm_enabled ? "#FFFF00 [开启]#" : "#FFFF00 [关闭]#"); 
            } else { 
                // 未编辑状态 
                sprintf(buf, "%02d:%02d\n%s", alarm_h, alarm_m, alarm_enabled ? "#00FF00 已开启#" : "右滑进入编辑"); 
            } 
        } 
    } 
    else if (sub_mode == 1) { 
        // ??【秒表界面】
        int min = (stopwatch_ms / 60000) % 60;
        int sec = (stopwatch_ms / 1000) % 60;
        int ms_100 = (stopwatch_ms % 1000) / 100;
        sprintf(buf, "%02d:%02d.%d\n%s", min, sec, ms_100, is_running ? "#00FF00 运行中#" : "#888888 已暂停#");
    } 
    else if (sub_mode == 2) { 
        // ?【世界时间界面】 
        time_t now; 
        time(&now); // 获取自 1970 年以来的秒数 (UTC) 
         
        // 核心魔法：当前 UTC 时间 + 目标城市的偏移秒数 
        time_t target_time = now + (world_cities[world_city_idx].offset * 3600); 
         
        struct tm target_tm; 
        // 使用 gmtime_r (获取格林威治标准时间)，避免受到系统时区 (TZ) 的干扰 
        gmtime_r(&target_time, &target_tm); 
         
        // 城市名标黄，下方显示 HH:MM:SS，最下方提示操作 
        sprintf(buf, "#FFFF00 %s#\n%02d:%02d:%02d\n上下切换城市", 
                world_cities[world_city_idx].name, 
                target_tm.tm_hour, target_tm.tm_min, target_tm.tm_sec); 
    } 
    else if (sub_mode == 3) { 
        // ?【倒计时界面】
        int min = (countdown_ms / 60000) % 99;
        int sec = (countdown_ms / 1000) % 60;
        
        // 核心：根据编辑状态，用黄色高亮对应的数字
        if (edit_state == 1) {
            sprintf(buf, "#FFFF00 %02d#:%02d\n上下调整分钟", min, sec);
        } else if (edit_state == 2) {
            sprintf(buf, "%02d:#FFFF00 %02d#\n上下调整秒数", min, sec);
        } else {
            sprintf(buf, "%02d:%02d.0\n%s", min, sec, is_running ? "#00FF00 运行中#" : "右滑进入编辑");
        }
    }
    else {
        sprintf(buf, "--:--\n正在开发中");
    }
    
    lv_label_set_text(label_clock_content, buf);
}

// ==========================================
// ?? 定时器心跳回调
// ==========================================
static void clock_timer_cb(lv_timer_t * timer) {
    // ? 新增：全局闹钟监听 (不受 is_running 和 view_level 的限制) 
    static uint8_t tick_1s = 0; 
    tick_1s++; 
    if (tick_1s >= 10) { // 100ms * 10 = 1秒钟查一次岗 
        tick_1s = 0; 
         
        // 获取底层真实系统时间 
        time_t now; 
        struct tm timeinfo; 
        time(&now); 
        localtime_r(&now, &timeinfo); 
 
        // 如果闹钟开启了，没在响，而且 时:分:00 秒 完美对齐 
        if (alarm_enabled && !alarm_is_ringing && 
            timeinfo.tm_hour == alarm_h && 
            timeinfo.tm_min == alarm_m && 
            timeinfo.tm_sec == 0) { 
             
            alarm_is_ringing = true; 
             
            // 霸道逻辑：无论用户在看啥，强行把屏幕切到闹钟界面！ 
            extern void switch_to_screen(ui_screen_state_t target); 
            switch_to_screen(SCREEN_CLOCK); 
             
            view_level = 1; // 强行进入详情层 
            sub_mode = 0;   // 强行切到闹钟子模式 
             
            // TODO: 在这里调用你的 app_mqtt 往外发通知，或者启动蜂鸣器 
            // app_mqtt_publish("jarvis/alarm", "RINGING"); 
             
            update_clock_display(); // 刷新屏幕变红 
        } 
    } 

    if (view_level == 0) return; // 如果在预览界面，不用狂刷屏幕 

    // ? 新增：如果是世界时间，每秒自动刷新一次屏幕，让秒钟跳起来！ 
    if (sub_mode == 2) { 
        static uint8_t wt_tick = 0; 
        wt_tick++; 
        if (wt_tick >= 10) { // 100ms * 10 = 1s 
            wt_tick = 0; 
            update_clock_display(); 
        } 
        return; 
    } 

    if (!is_running) return;

    if (sub_mode == 1) { // 秒表
        stopwatch_ms += 100;
        update_clock_display();
    } 
    else if (sub_mode == 3) { // 倒计时
        if (countdown_ms >= 100) {
            countdown_ms -= 100;
            update_clock_display();
        } else {
            is_running = false;
            lv_label_set_recolor(label_clock_content, true);
            lv_label_set_text(label_clock_content, "#FF0000 00:00.0\n时间到！#");
        }
    }
}

// ==========================================
// ? 核心指令路由
// ==========================================
void clock_screen_handle_cmd(ui_cmd_t cmd) {
    if (view_level == 0) {
        if (cmd == UI_CMD_UP)   sub_mode = (sub_mode + 3) % 4; 
        if (cmd == UI_CMD_DOWN) sub_mode = (sub_mode + 1) % 4; 
        if (cmd == UI_CMD_RIGHT) view_level = 1; 
        if (cmd == UI_CMD_LEFT) {
            extern void switch_to_screen(ui_screen_state_t target);
            switch_to_screen(SCREEN_MENU);
        }
        update_clock_display();
        return;
    }

    // ------------------------------------
    // 层级 1：真实功能操作模式 
    // ------------------------------------

    // ?【闹钟逻辑】 
    if (sub_mode == 0) { 
        // 如果正在响铃，随便按左或右都可以关闭闹钟！ 
        if (alarm_is_ringing) { 
            if (cmd == UI_CMD_LEFT || cmd == UI_CMD_RIGHT) { 
                alarm_is_ringing = false; 
                alarm_enabled = false; // 响完自动关掉 
                view_level = 0;        // 退回预览层 
                update_clock_display(); 
            } 
            return; // 响铃时屏蔽其他操作 
        } 
 
        if (edit_state == 0) { // 未编辑状态 
            if (cmd == UI_CMD_RIGHT) edit_state = 1; // 右滑进入编辑 (改小时) 
            if (cmd == UI_CMD_LEFT)  view_level = 0; // 左滑退回预览 
        } 
        else { // 正在编辑中 
            if (cmd == UI_CMD_UP || cmd == UI_CMD_DOWN) { 
                int offset = (cmd == UI_CMD_UP) ? 1 : -1; 
                 
                if (edit_state == 1) { // 调小时 (0-23) 
                    if (offset > 0) alarm_h = (alarm_h + 1) % 24; 
                    else alarm_h = (alarm_h == 0) ? 23 : alarm_h - 1; 
                } 
                else if (edit_state == 2) { // 调分钟 (0-59) 
                    if (offset > 0) alarm_m = (alarm_m + 1) % 60; 
                    else alarm_m = (alarm_m == 0) ? 59 : alarm_m - 1; 
                } 
                else if (edit_state == 3) { // 调开关 
                    alarm_enabled = !alarm_enabled; 
                } 
            } 
            if (cmd == UI_CMD_RIGHT) { 
                edit_state++; // 焦点：小时 -> 分钟 -> 开关 -> 完成 
                if (edit_state > 3) edit_state = 0; // 退出编辑 
            } 
            if (cmd == UI_CMD_LEFT) { 
                edit_state = 0; // 随时取消编辑 
            } 
        } 
    } 
    // ?【世界时间逻辑】 
    else if (sub_mode == 2) { 
        if (cmd == UI_CMD_UP) { 
            // 上挥：切换上一个城市 (处理索引 0 的下溢问题) 
            world_city_idx = (world_city_idx == 0) ? WORLD_CITY_COUNT - 1 : world_city_idx - 1; 
        } 
        else if (cmd == UI_CMD_DOWN) { 
            // 下挥：切换下一个城市 
            world_city_idx = (world_city_idx + 1) % WORLD_CITY_COUNT; 
        } 
        else if (cmd == UI_CMD_LEFT) { 
            // 左挥：退出到预览界面 
            view_level = 0; 
        } 
        // 右挥不需要做特别处理，可以在这里直接屏蔽 
    } 
    else if (sub_mode == 3) { // 倒计时
        if (edit_state == 0) { 
            if (cmd == UI_CMD_RIGHT) {
                if (!is_running) edit_state = 1; 
                else is_running = false; 
            }
            if (cmd == UI_CMD_LEFT) {
                view_level = 0; 
                is_running = false;
            }
        } 
        else { 
            if (cmd == UI_CMD_UP || cmd == UI_CMD_DOWN) {
                int offset = (cmd == UI_CMD_UP) ? 1 : -1;
                if (edit_state == 1) countdown_ms += offset * 60 * 1000;
                if (edit_state == 2) countdown_ms += offset * 1000;      
                if (countdown_ms > 99 * 60 * 1000) countdown_ms = 0;     
            }
            if (cmd == UI_CMD_RIGHT) {
                edit_state++; 
                if (edit_state > 2) {
                    edit_state = 0; 
                    is_running = true; 
                }
            }
            if (cmd == UI_CMD_LEFT) edit_state = 0; 
        }
    }
    else if (sub_mode == 1) { // 秒表
        if (cmd == UI_CMD_RIGHT) is_running = !is_running; 
        if (cmd == UI_CMD_LEFT) {
            if (is_running) is_running = false; 
            else if (stopwatch_ms > 0) stopwatch_ms = 0; 
            else view_level = 0; 
        }
    }
    else {
        if (cmd == UI_CMD_LEFT) view_level = 0;
    }

    update_clock_display();
}

// ==========================================
// ? 初始化界面
// ==========================================
void ui_clock_screen_init(void) {
    ui_clock_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_clock_screen, lv_color_black(), 0);

    label_clock_title = lv_label_create(ui_clock_screen);
    lv_obj_set_style_text_font(label_clock_title, &my_font_cn_16, 0); // 用你的全能字库
    lv_obj_set_style_text_color(label_clock_title, lv_color_hex(0x00FF00), 0);
    lv_obj_align(label_clock_title, LV_ALIGN_TOP_MID, 0, 20);

    label_clock_content = lv_label_create(ui_clock_screen);
    lv_obj_set_style_text_font(label_clock_content, &my_font_cn_16, 0); // 也是你的字库
    lv_obj_set_style_text_color(label_clock_content, lv_color_white(), 0);
    lv_obj_align(label_clock_content, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(label_clock_content, LV_TEXT_ALIGN_CENTER, 0);
    
    // 行间距调大一点，让数字和下方提示不那么挤
    lv_obj_set_style_text_line_space(label_clock_content, 10, 0);

    view_level = 0;
    sub_mode = 3; 
    update_clock_display();

    timer_handle = lv_timer_create(clock_timer_cb, 100, NULL);
}