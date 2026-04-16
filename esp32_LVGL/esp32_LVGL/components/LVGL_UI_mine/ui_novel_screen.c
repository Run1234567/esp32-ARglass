#include "ui_novel_screen.h"
#include "ui_globals.h"

#include <string.h>
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_mqtt.h"

#define SCREEN_ROWS 11      // 屏幕显示的行数
#define ROW_WIDTH 40        // 每行最大字节
uint8_t novel_scroll_task_running = 0; // 滚动任务状态标志
// 1. 书库缓冲区 (用来存从网络或SD卡收到的几千字)
char * novel_source_buffer =  NULL;

size_t current_book_pos = 0;    // ✨ “书签”：记录读到哪里了

// 2. 显示队列 (二维数组，每一行是一个字符串)
static char display_lines[SCREEN_ROWS][ROW_WIDTH];
static char full_display_str[SCREEN_ROWS * ROW_WIDTH]; // 最终喂给LVGL的合体字符串
// ==========================================
// ✨ 核心函数：从书库取一行，并滚动显示 (精准切片版)
// ==========================================
void novel_scroll_one_line(void) {
    if (novel_source_buffer == NULL || novel_source_buffer[current_book_pos] == '\0') {
        novel_scroll_task_running=1;
        app_mqtt_publish("jarvis/glasses/book", "novel_end"); // 书读完了，通知 Python 脚本可以发下一章了
        return; // 没书或者读完了，直接返回
    }

    // 1. 【推陈】：将当前显示的 1~11 行向上平移
    for (int i = 0; i < SCREEN_ROWS - 1; i++) {
        // 使用 strncpy 更安全，防止数组越界导致内存崩溃
        strncpy(display_lines[i], display_lines[i + 1], ROW_WIDTH - 1);
        display_lines[i][ROW_WIDTH - 1] = '\0'; // 强行封口
    }

    // 2. 【取新】：从书库缓冲区提取下一行
    char next_line[ROW_WIDTH];
    int line_len = 0;

    // 智能截取（处理UTF-8，防止截断，并过滤垃圾字符）
    while (novel_source_buffer[current_book_pos] != '\0') {
        
        // ✨ 关键修复 1：无情击杀 Windows 的 '\r' 回车符
        if (novel_source_buffer[current_book_pos] == '\r') {
            current_book_pos++;
            continue;
        }

        // 遇到正式的换行符 '\n'
        if (novel_source_buffer[current_book_pos] == '\n') {
            current_book_pos++; // 指针越过 '\n'，留给下一次读取
            break; // 这一行到此结束
        }

        // 识别当前 UTF-8 字符到底占几个字节
        int char_bytes = 1;
        unsigned char c = (unsigned char)novel_source_buffer[current_book_pos];
        if (c < 0x80) char_bytes = 1;       // 英文标点 (1字节)
        else if (c < 0xE0) char_bytes = 2;  // 拉丁文 (2字节)
        else if (c < 0xF0) char_bytes = 3;  // 中文 (3字节)
        else char_bytes = 4;                // 罕见字/Emoji (4字节)

        // ✨ 关键修复 2：严格根据物理宽度换行
        // 如果加上这一个字，长度就塞爆 ROW_WIDTH 了
        if (line_len + char_bytes >= ROW_WIDTH - 1) {
            break; // 立刻刹车！这个字不要了，留给下一次调用 novel_scroll_one_line 时再读
        }

        // 安全拷贝这个完整的字符
        for (int j = 0; j < char_bytes; j++) {
            next_line[line_len++] = novel_source_buffer[current_book_pos++];
        }
    }
    
    // 安全封口，构成标准 C 语言字符串
    next_line[line_len] = '\0'; 

    // 3. 【出新】：放入显示队列的最后一行
    strncpy(display_lines[SCREEN_ROWS - 1], next_line, ROW_WIDTH - 1);
    display_lines[SCREEN_ROWS - 1][ROW_WIDTH - 1] = '\0';

    // 4. 【刷新】：合并字符串并更新 LVGL 屏幕
    full_display_str[0] = '\0'; // 清空合体字符串
    for (int i = 0; i < SCREEN_ROWS; i++) {
        // 防止拼接空数据
        strcat(full_display_str, display_lines[i]);
        strcat(full_display_str, "\n");
    }

    // 上锁更新 UI
    if (lvgl_port_lock(0)) {
        lv_label_set_text(label_novel_text, full_display_str);
        lvgl_port_unlock();
    }
}


// 🌍 定义全局对象
lv_obj_t * ui_novel_screen;
lv_obj_t * label_novel_text;
lv_obj_t * novel_scroll_cont; // 滚动容器也提出来，方便以后用代码让它滚动

const char * test_novel_text = 
    "第一章 陨落的天才\n\n"
    "“斗之力，三段！”\n\n"
    "望着测验魔石碑上面闪亮得甚至有些刺眼的五个大字，少年面无表情，唇角有着一抹自嘲，紧握的手掌，因为大力，而导致略微尖锐的指甲深深的刺进了掌心之中，带来一阵阵钻心的疼痛...\n\n"
    "“萧炎，斗之力，三段！级别：低级！”测验魔石碑之旁，一位中年男子，看了一眼碑上所显示出来的信息，语气漠然的将之公布了出来...\n\n"
    "中年男子话刚刚脱口，便是不出意外的在人头汹涌的广场中带起了一阵嘲讽的骚动。\n\n"
    "“三段？嘿嘿，果然不出我所料，这个‘天才’这一年又是在原地踏步！”\n\n"
    "“哎，这废物真是把家族的脸都给丢光了。”\n\n"
    "“要不是族长是他的父亲，这种废物，早就被驱赶出家族，任其自生自灭了，哪还有机会待在家族中白吃白喝。”\n\n"
    "“唉，昔年那名震乌坦城的天才少年，如今怎么落魄成这般模样了啊？”\n\n"
    "“谁知道呢，或许做了什么亏心事，惹得神灵降怒了吧...”\n\n"
    "周围传来的不屑嘲笑以及惋惜轻叹，落在那如木桩般伫立在原地的少年耳中，恍如一根根利刺狠狠的扎在心脏一般，让得少年呼吸微微急促。\n\n"
    "少年缓缓抬起头来，露出一张有些清秀的稚嫩脸庞，漆黑的眸子木然的在周围那些嘲讽的同龄人身上扫过，少年嘴角的自嘲，似乎变得更加苦涩了。";

void ui_novel_screen_init(void) {
    // 1. 创建基础屏幕
    ui_novel_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_novel_screen, lv_color_black(), 0); 
    
    // 2. 创建滚动容器 (限定在屏幕范围内)
    novel_scroll_cont = lv_obj_create(ui_novel_screen);
    lv_obj_set_size(novel_scroll_cont, 240, 240); // 假设屏幕是 240x240
    lv_obj_center(novel_scroll_cont);
    
    // 设置容器样式：黑底、无边框、去掉默认的内边距(Padding)留出更多显示空间
    lv_obj_set_style_bg_color(novel_scroll_cont, lv_color_black(), 0);
    lv_obj_set_style_border_width(novel_scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(novel_scroll_cont, 5, 0); 
    
    // ⚠️ 关键设置：只允许垂直滚动，隐藏或自动显示滚动条
    lv_obj_set_scroll_dir(novel_scroll_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(novel_scroll_cont, LV_SCROLLBAR_MODE_AUTO);

    // 3. 创建小说文本标签
    label_novel_text = lv_label_create(novel_scroll_cont);
    
    // ⚠️ 极其关键：必须限制宽度，否则文字会一直往右跑跑到屏幕外面
    lv_obj_set_width(label_novel_text, 220); // 留一点边距给滚动条
    
    // 开启自动换行模式
    lv_label_set_long_mode(label_novel_text, LV_LABEL_LONG_WRAP); 
    
    // 应用你刚刚辛苦编译出来的 3500+ 中文字库！
    lv_obj_set_style_text_font(label_novel_text, &my_font_cn_16, 0); 
    lv_obj_set_style_text_color(label_novel_text, lv_color_white(), 0);
    
    // 设置行距，让阅读更舒服一点
    lv_obj_set_style_text_line_space(label_novel_text, 6, 0);
    
    // 初始提示语
    lv_label_set_text(label_novel_text, test_novel_text);
}