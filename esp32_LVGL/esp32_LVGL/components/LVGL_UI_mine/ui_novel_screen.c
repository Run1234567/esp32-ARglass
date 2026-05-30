#include "ui_novel_screen.h"
#include "ui_globals.h"

#include <string.h>
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "my_uart.h"
#include "esp_log.h"

#define SCREEN_ROWS 11
#define ROW_WIDTH 40
uint8_t novel_scroll_task_running = 0;
char * novel_source_buffer = NULL;
size_t current_book_pos = 0;

static lv_timer_t * auto_scroll_timer = NULL;
static lv_obj_t * panel_settings = NULL;
static lv_obj_t * label_settings = NULL;

static uint8_t is_in_settings = 0;
static uint8_t setting_focus = 0;

// ✨ 核心设计：三大阅读模式
uint8_t novel_read_mode = 0; // 0=手动翻页, 1=自动翻页, 2=语音同步

// ✨ 核心设计：档位配置表
static const int speed_options_ms[] = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000};
static const int num_speeds = sizeof(speed_options_ms) / sizeof(speed_options_ms[0]);
static int auto_speed_idx = 9; // 默认挂在 1000ms (1秒)
static int tts_speed_val = 4;  // 默认语速 4 (0-9)

static char display_lines[SCREEN_ROWS][ROW_WIDTH];
static char full_display_str[SCREEN_ROWS * ROW_WIDTH];

// ✨ 三级菜单状态
typedef enum {
    NOVEL_STATE_BOOK_LIST,
    NOVEL_STATE_CHAP_LIST,
    NOVEL_STATE_READING
} novel_ui_state_t;
static novel_ui_state_t ui_state = NOVEL_STATE_BOOK_LIST;

// ✨ 书单和章节列表对象
static lv_obj_t * novel_book_list = NULL;
static lv_obj_t * novel_chap_list = NULL;
static char current_selected_book[64] = "";

// ✨ 分页状态记录
static int current_book_offset = 0;
static int current_chap_offset = 0;

// ==========================================
// ✨ 核心修复：专为魔杖定制的列表选择器
// ==========================================
static void list_move_selection(lv_obj_t * list, int direction) {
    if (lvgl_port_lock(0)) {
        uint32_t cnt = lv_obj_get_child_cnt(list);
        if (cnt > 0) {
            int current_sel = -1;
            // 找到当前亮起（FOCUSED）的那一行
            for(uint32_t i = 0; i < cnt; i++) {
                lv_obj_t * child = lv_obj_get_child(list, i);
                if(lv_obj_has_state(child, LV_STATE_FOCUSED)) {
                    current_sel = i;
                    break;
                }
            }

            // 清除旧高亮
            if (current_sel >= 0) {
                lv_obj_clear_state(lv_obj_get_child(list, current_sel), LV_STATE_FOCUSED);
            }

            // 计算新光标位置
            int new_sel = current_sel + direction;
            if (new_sel < 0) new_sel = 0;
            if (new_sel >= cnt) new_sel = cnt - 1;

            // 点亮新高亮并自动滚动屏幕
            lv_obj_t * new_child = lv_obj_get_child(list, new_sel);
            lv_obj_add_state(new_child, LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(new_child, LV_ANIM_ON);
        }
        lvgl_port_unlock();
    }
}

static void list_click_selected(lv_obj_t * list, void (*cb)(lv_event_t *)) {
    if (lvgl_port_lock(0)) {
        uint32_t cnt = lv_obj_get_child_cnt(list);
        if (cnt > 0) {
            lv_obj_t * selected = NULL;
            // 寻找当前高亮的行
            for(uint32_t i = 0; i < cnt; i++) {
                lv_obj_t * child = lv_obj_get_child(list, i);
                if(lv_obj_has_state(child, LV_STATE_FOCUSED)) {
                    selected = child;
                    break;
                }
            }

            // ⚠️ 极其关键：如果你没下滑就直接右滑，默认选中第一项！
            if (!selected) {
                selected = lv_obj_get_child(list, 0);
                lv_obj_add_state(selected, LV_STATE_FOCUSED);
            }

            // 强制触发点击回调函数
            if (selected) {
                lv_event_t e;
                e.target = selected;
                cb(&e);
            }
        }
        lvgl_port_unlock();
    }
}


// ==========================================
//   分页按钮回调函数
// ==========================================
static void book_prev_cb(lv_event_t * e) {
    current_book_offset -= 6;
    if(current_book_offset < 0) current_book_offset = 0;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "CMD:GET_BOOKS:%d\r\n", current_book_offset);
    my_uart_send(cmd);
}

static void book_next_cb(lv_event_t * e) {
    current_book_offset += 6;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "CMD:GET_BOOKS:%d\r\n", current_book_offset);
    my_uart_send(cmd);
}

static void chap_prev_cb(lv_event_t * e) {
    current_chap_offset -= 6;
    if(current_chap_offset < 0) current_chap_offset = 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "CMD:GET_CHAPS:%s,%d\r\n", current_selected_book, current_chap_offset);
    my_uart_send(cmd);
}

static void chap_next_cb(lv_event_t * e) {
    current_chap_offset += 6;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "CMD:GET_CHAPS:%s,%d\r\n", current_selected_book, current_chap_offset);
    my_uart_send(cmd);
}

// ==========================================
// 核心函数：从书库取一行，并滚动显示
// ==========================================
void novel_scroll_one_line(void) {
    if (novel_source_buffer == NULL) {
        if (!novel_scroll_task_running && novel_read_mode != 2) {
            novel_scroll_task_running = 1;
            my_uart_send("CMD:NOVEL_END\r\n");
        }
        return;
    }

    int remaining = strlen(&novel_source_buffer[current_book_pos]);
    char *newline_ptr = strchr(&novel_source_buffer[current_book_pos], '\n');

    // ✨ 半截行拦截逻辑：如果是语音同步模式 (2)，哪怕只有半截字也必须立刻显示！
    if (remaining < ROW_WIDTH - 1 && newline_ptr == NULL) {
        if (novel_read_mode != 2) {
            if (!novel_scroll_task_running) {
                novel_scroll_task_running = 1;
                my_uart_send("CMD:NOVEL_END\r\n");
            }
            return;
        }
    }
    if (novel_source_buffer[current_book_pos] == '\0') return;

    for (int i = 0; i < SCREEN_ROWS - 1; i++) {
        strncpy(display_lines[i], display_lines[i + 1], ROW_WIDTH - 1);
        display_lines[i][ROW_WIDTH - 1] = '\0';
    }

    char next_line[ROW_WIDTH];
    int line_len = 0;

    while (novel_source_buffer[current_book_pos] != '\0') {
        if (novel_source_buffer[current_book_pos] == '\r') { current_book_pos++; continue; }
        if (novel_source_buffer[current_book_pos] == '\n') { current_book_pos++; break; }

        int char_bytes = 1;
        unsigned char c = (unsigned char)novel_source_buffer[current_book_pos];
        if (c < 0x80) char_bytes = 1;
        else if (c < 0xE0) char_bytes = 2;
        else if (c < 0xF0) char_bytes = 3;
        else char_bytes = 4;

        if (line_len + char_bytes >= ROW_WIDTH - 1) break;

        for (int j = 0; j < char_bytes; j++) {
            next_line[line_len++] = novel_source_buffer[current_book_pos++];
        }
    }

    next_line[line_len] = '\0';
    strncpy(display_lines[SCREEN_ROWS - 1], next_line, ROW_WIDTH - 1);
    display_lines[SCREEN_ROWS - 1][ROW_WIDTH - 1] = '\0';

    full_display_str[0] = '\0';
    for (int i = 0; i < SCREEN_ROWS; i++) {
        strcat(full_display_str, display_lines[i]);
        strcat(full_display_str, "\n");
    }

    if (lvgl_port_lock(0)) {
        lv_label_set_text(label_novel_text, full_display_str);
        lvgl_port_unlock();
    }
}

// ✨ 为语音模式准备的"一拉到底"函数
void novel_scroll_all_buffer(void) {
    while(novel_source_buffer != NULL && novel_source_buffer[current_book_pos] != '\0') {
        novel_scroll_one_line();
    }
}

// ==========================================
// 自动翻页定时器回调
// ==========================================
static void auto_scroll_timer_cb(lv_timer_t * timer) {
    // 只有在模式 1 (自动模式) 下，定时器才干活
    if (!is_in_settings && novel_read_mode == 1 && ui_state == NOVEL_STATE_READING) {
        novel_scroll_one_line();
    }
}

// ==========================================
// 刷新悬浮设置面板的显示
// ==========================================
static void update_novel_settings_display(void) {
    char buf[256];
    char mode_str[32];
    char speed_str[32];

    if (novel_read_mode == 0) {
        strcpy(mode_str, "手动无声");
        strcpy(speed_str, "---");
    } else if (novel_read_mode == 1) {
        strcpy(mode_str, "自动翻页");
        int ms = speed_options_ms[auto_speed_idx];
        if (ms < 1000) sprintf(speed_str, "0.%d秒/行", ms / 100);
        else sprintf(speed_str, "%d秒/行", ms / 1000);
    } else {
        strcpy(mode_str, "语音同步");
        sprintf(speed_str, "语速: %d", tts_speed_val);
    }

    if (setting_focus == 0) {
        sprintf(buf, "⚙️ 阅读设置\n\n#FFFF00 > 模式: %s <#\n  速度: %s  \n\n上下调节 右滑切换", mode_str, speed_str);
    } else {
        sprintf(buf, "⚙️ 阅读设置\n\n  模式: %s  \n#FFFF00 > 速度: %s <#\n\n上下调节 右滑切换", mode_str, speed_str);
    }
    lv_label_set_text(label_settings, buf);
}

// ==========================================
// 📡 供串口调用的 UI 刷新接口
// ==========================================
void novel_ui_clear_book_list(void) {
    if (lvgl_port_lock(0)) {
        lv_obj_clean(novel_book_list);
        lvgl_port_unlock();
    }
}

void novel_ui_add_book(const char* name) {
    if (lvgl_port_lock(0)) {
        lv_obj_t * btn = lv_list_add_btn(novel_book_list, LV_SYMBOL_DIRECTORY, name);
        lv_obj_set_style_text_font(btn, &my_font_cn_16, 0);
        // ✨ 当被选中时，变成深蓝色，非常显眼！
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0055AA), LV_STATE_FOCUSED); 
        lvgl_port_unlock();
    }
}

void novel_ui_clear_chap_list(void) {
    if (lvgl_port_lock(0)) {
        lv_obj_clean(novel_chap_list);
        lvgl_port_unlock();
    }
}

void novel_ui_add_chap(const char* name) {
    if (lvgl_port_lock(0)) {
        lv_obj_t * btn = lv_list_add_btn(novel_chap_list, LV_SYMBOL_FILE, name);
        lv_obj_set_style_text_font(btn, &my_font_cn_16, 0);
        // ✨ 当被选中时，变成深蓝色
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0055AA), LV_STATE_FOCUSED);
        lvgl_port_unlock();
    }
}

// ==========================================
//   供 my_uart.c 调用的添加分页按钮接口
// ==========================================
void novel_ui_add_book_page_btn(int is_next) {
    if (lvgl_port_lock(0)) {
        lv_obj_t * btn;
        if (is_next) {
            btn = lv_list_add_btn(novel_book_list, LV_SYMBOL_DOWN, "下一页");
            lv_obj_add_event_cb(btn, book_next_cb, LV_EVENT_CLICKED, NULL);
        } else {
            btn = lv_list_add_btn(novel_book_list, LV_SYMBOL_UP, "上一页");
            lv_obj_add_event_cb(btn, book_prev_cb, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_set_style_text_font(btn, &my_font_cn_16, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0055AA), LV_STATE_FOCUSED);
        lvgl_port_unlock();
    }
}

void novel_ui_add_chap_page_btn(int is_next) {
    if (lvgl_port_lock(0)) {
        lv_obj_t * btn;
        if (is_next) {
            btn = lv_list_add_btn(novel_chap_list, LV_SYMBOL_DOWN, "下一页");
            lv_obj_add_event_cb(btn, chap_next_cb, LV_EVENT_CLICKED, NULL);
        } else {
            btn = lv_list_add_btn(novel_chap_list, LV_SYMBOL_UP, "上一页");
            lv_obj_add_event_cb(btn, chap_prev_cb, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_set_style_text_font(btn, &my_font_cn_16, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0055AA), LV_STATE_FOCUSED);
        lvgl_port_unlock();
    }
}

// ==========================================
// 切换三级菜单视图
// ==========================================
static void switch_novel_view(novel_ui_state_t new_state) {
    lv_obj_add_flag(novel_book_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(novel_chap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(novel_scroll_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);
    is_in_settings = 0;

    switch (new_state) {
        case NOVEL_STATE_BOOK_LIST:
            lv_obj_clear_flag(novel_book_list, LV_OBJ_FLAG_HIDDEN);
            break;
        case NOVEL_STATE_CHAP_LIST:
            lv_obj_clear_flag(novel_chap_list, LV_OBJ_FLAG_HIDDEN);
            break;
        case NOVEL_STATE_READING:
            lv_obj_clear_flag(novel_scroll_cont, LV_OBJ_FLAG_HIDDEN);
            break;
    }
    ui_state = new_state;
}

// ==========================================
// 事件回调：当点击某本书时
// ==========================================
static void book_list_btn_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    const char * book_name = lv_list_get_btn_text(novel_book_list, btn);

    strncpy(current_selected_book, book_name, sizeof(current_selected_book) - 1);
    current_selected_book[sizeof(current_selected_book) - 1] = '\0';

    switch_novel_view(NOVEL_STATE_CHAP_LIST);

    current_chap_offset = 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "CMD:GET_CHAPS:%s,0\r\n", book_name);
    my_uart_send(cmd);
}

// ==========================================
// 事件回调：当点击某个章节时
// ==========================================
static void chap_list_btn_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    const char * chap_name = lv_list_get_btn_text(novel_chap_list, btn);

    switch_novel_view(NOVEL_STATE_READING);

    lv_label_set_text(label_novel_text, "加载中...");

    // ✨ 修复 1：进新章节前，彻底清空上一章的残余文字，防止两章内容粘连！
    if (lvgl_port_lock(0)) {
        if (novel_source_buffer != NULL) {
            free(novel_source_buffer);
            novel_source_buffer = NULL;
        }
        current_book_pos = 0;
        lvgl_port_unlock();
    }

    // ✨ 修复 2：每次进小说，强行将 UI 和主板重置为"手动无声"模式 (0)
    novel_read_mode = 0;
    my_uart_send("CMD:MODE:TEXT\r\n");

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CMD:READ_CHAP:%s/%s\r\n", current_selected_book, chap_name);
    my_uart_send(cmd);
}

// ==========================================
// 小说模块专属手势路由 (✨ 完美适配魔杖)
// ==========================================
void novel_screen_handle_cmd(ui_cmd_t cmd) {
    switch (ui_state) {
        // ------------------------------------
        // 状态 A：书单列表
        // ------------------------------------
        case NOVEL_STATE_BOOK_LIST:
            if (cmd == UI_CMD_LEFT) {
                extern void switch_to_screen(ui_screen_state_t target);
                switch_to_screen(SCREEN_MENU);
            }
            else if (cmd == UI_CMD_UP) {
                list_move_selection(novel_book_list, -1);
            }
            else if (cmd == UI_CMD_DOWN) {
                list_move_selection(novel_book_list, 1);
            }
            else if (cmd == UI_CMD_RIGHT) {
                list_click_selected(novel_book_list, book_list_btn_cb);
            }
            break;

        // ------------------------------------
        // 状态 B：章节列表
        // ------------------------------------
        case NOVEL_STATE_CHAP_LIST:
            if (cmd == UI_CMD_LEFT) {
                switch_novel_view(NOVEL_STATE_BOOK_LIST);
            }
            else if (cmd == UI_CMD_UP) {
                list_move_selection(novel_chap_list, -1);
            }
            else if (cmd == UI_CMD_DOWN) {
                list_move_selection(novel_chap_list, 1);
            }
            else if (cmd == UI_CMD_RIGHT) {
                list_click_selected(novel_chap_list, chap_list_btn_cb);
            }
            break;

        // ------------------------------------
        // 状态 C：阅读中
        // ------------------------------------
        case NOVEL_STATE_READING:
            if (is_in_settings == 0) {
                if (cmd == UI_CMD_DOWN) {
                    // ✨ 语音模式下，屏蔽手动下滑翻页！
                    if (novel_read_mode != 2) {
                        novel_scroll_one_line();
                    }
                }
                else if (cmd == UI_CMD_LEFT) {
                    switch_novel_view(NOVEL_STATE_CHAP_LIST);
                    // ✨ 退出阅读时，发信号叫停主板的 TTS
                    my_uart_send("CMD:STOP_READING\r\n");
                }
                else if (cmd == UI_CMD_RIGHT) {
                    is_in_settings = 1;
                    setting_focus = 0;
                    lv_obj_clear_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);
                    update_novel_settings_display();
                    lv_timer_pause(auto_scroll_timer);
                }
            }
            else {
                if (cmd == UI_CMD_RIGHT) {
                    // 如果是手动模式，速度项不可选
                    if (novel_read_mode != 0) {
                        setting_focus = (setting_focus + 1) % 2;
                    }
                    update_novel_settings_display();
                }
                else if (cmd == UI_CMD_UP || cmd == UI_CMD_DOWN) {
                    int offset = (cmd == UI_CMD_UP) ? 1 : -1;

                    if (setting_focus == 0) {
                        int temp_mode = novel_read_mode + offset;
                        if (temp_mode > 2) temp_mode = 2;
                        if (temp_mode < 0) temp_mode = 0;
                        
                        // ✨ 修复 3：如果在设置里切换了模式，必须踢主板一脚！
                        if (novel_read_mode != temp_mode) {
                            novel_read_mode = temp_mode;
                            if (novel_read_mode == 2) {
                                my_uart_send("CMD:MODE:TTS\r\n");
                                // 🌟 从无声切到语音，TTS碗里没字，立刻发个催更信号让它去读第一句！
                                my_uart_send("CMD:NOVEL_END\r\n");
                            }
                            else {
                                my_uart_send("CMD:MODE:TEXT\r\n");
                                // 🌟 从语音切回无声，也发个信号让屏幕马上有字显示
                                my_uart_send("CMD:NOVEL_END\r\n");
                            }
                        }
                    }
                    else if (setting_focus == 1) {
                        if (novel_read_mode == 1) {
                            auto_speed_idx += offset;
                            if (auto_speed_idx >= num_speeds) auto_speed_idx = num_speeds - 1;
                            if (auto_speed_idx < 0) auto_speed_idx = 0;
                        } else if (novel_read_mode == 2) {
                            tts_speed_val += offset;
                            if (tts_speed_val > 9) tts_speed_val = 9;
                            if (tts_speed_val < 0) tts_speed_val = 0;
                            // 语速变成立刻通知主板
                            char cmd_buf[32];
                            sprintf(cmd_buf, "CMD:TTS_SPEED:%d\r\n", tts_speed_val);
                            my_uart_send(cmd_buf);
                        }
                    }
                    update_novel_settings_display();
                }
                else if (cmd == UI_CMD_LEFT) {
                    is_in_settings = 0;
                    lv_obj_add_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);
                    // 如果是自动模式，恢复并应用新速度
                    if (novel_read_mode == 1) {
                        lv_timer_set_period(auto_scroll_timer, speed_options_ms[auto_speed_idx]);
                        lv_timer_resume(auto_scroll_timer);
                    }
                }
            }
            break;
    }
}

// ==========================================
// 全局对象
// ==========================================
lv_obj_t * ui_novel_screen;
lv_obj_t * label_novel_text;
lv_obj_t * novel_scroll_cont;

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

// ==========================================
// 初始化界面
// ==========================================
void ui_novel_screen_init(void) {
    ui_novel_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_novel_screen, lv_color_black(), 0);

    // --- 1. 创建书名列表 (默认显示) ---
    novel_book_list = lv_list_create(ui_novel_screen);
    lv_obj_set_size(novel_book_list, 240, 240);
    lv_obj_center(novel_book_list);
    lv_obj_set_style_bg_color(novel_book_list, lv_color_black(), 0);
    lv_obj_set_style_text_color(novel_book_list, lv_color_white(), 0);
    lv_obj_set_style_text_font(novel_book_list, &my_font_cn_16, 0);

    // --- 2. 创建章节列表 (默认隐藏) ---
    novel_chap_list = lv_list_create(ui_novel_screen);
    lv_obj_set_size(novel_chap_list, 240, 240);
    lv_obj_center(novel_chap_list);
    lv_obj_set_style_bg_color(novel_chap_list, lv_color_black(), 0);
    lv_obj_set_style_text_color(novel_chap_list, lv_color_white(), 0);
    lv_obj_set_style_text_font(novel_chap_list, &my_font_cn_16, 0);
    lv_obj_add_flag(novel_chap_list, LV_OBJ_FLAG_HIDDEN);

    // --- 3. 创建阅读区 (默认隐藏) ---
    novel_scroll_cont = lv_obj_create(ui_novel_screen);
    lv_obj_set_size(novel_scroll_cont, 240, 240);
    lv_obj_center(novel_scroll_cont);
    lv_obj_set_style_bg_color(novel_scroll_cont, lv_color_black(), 0);
    lv_obj_set_style_border_width(novel_scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(novel_scroll_cont, 5, 0);
    lv_obj_set_scroll_dir(novel_scroll_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(novel_scroll_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(novel_scroll_cont, LV_OBJ_FLAG_HIDDEN);

    label_novel_text = lv_label_create(novel_scroll_cont);
    lv_obj_set_width(label_novel_text, 220);
    lv_label_set_long_mode(label_novel_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label_novel_text, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_novel_text, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(label_novel_text, 6, 0);
    lv_label_set_text(label_novel_text, test_novel_text);

    // --- 4. 创建悬浮设置面板 (默认隐藏) ---
    panel_settings = lv_obj_create(ui_novel_screen);
    lv_obj_set_size(panel_settings, 180, 120);
    lv_obj_center(panel_settings);
    lv_obj_set_style_bg_color(panel_settings, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_color(panel_settings, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel_settings, 2, 0);
    lv_obj_add_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);

    label_settings = lv_label_create(panel_settings);
    lv_obj_center(label_settings);
    lv_label_set_recolor(label_settings, true);
    lv_obj_set_style_text_align(label_settings, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label_settings, &my_font_cn_16, 0);

    // --- 5. 创建自动翻页定时器 (初始暂停) ---
    auto_scroll_timer = lv_timer_create(auto_scroll_timer_cb, 3000, NULL);
    lv_timer_pause(auto_scroll_timer);

    // 初始状态：书单列表
    ui_state = NOVEL_STATE_BOOK_LIST;
}