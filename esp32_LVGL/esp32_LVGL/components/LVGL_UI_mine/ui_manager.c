#include "ui_manager.h"
#include "ui_globals.h"
#include "esp_log.h"
#include "esp_lvgl_port.h" // 使用官方的锁

// 引入你写好的所有屏幕模块
#include "ui_ar_glass.h"
#include "ui_menu_screen.h"
#include "ui_novel_screen.h"
#include "ui_clock_screen.h" // ? 新增

static const char *TAG = "UI_MANAGER";

// 实例化队列和当前状态
QueueHandle_t ui_cmd_queue = NULL;
static ui_screen_state_t current_screen = SCREEN_MAIN_AR;

// =========================================================
// ? 屏幕切换引擎 (带动画和线程锁)
// =========================================================
void switch_to_screen(ui_screen_state_t target_screen) {
    if (current_screen == target_screen) return;

    lv_obj_t * target_obj = NULL;

    switch (target_screen) {
        case SCREEN_MAIN_AR: target_obj = ui_main_screen; break;
        case SCREEN_MENU:    target_obj = ui_menu_screen; break;
        case SCREEN_NOVEL:   target_obj = ui_novel_screen; break;
        case SCREEN_CLOCK:   target_obj = ui_clock_screen; break; // ? 新增
        default: return;
    }

    // 切换屏幕 (无动画，AR 眼镜追求瞬间响应)
    lv_scr_load_anim(target_obj, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    current_screen = target_screen;
    ESP_LOGI(TAG, "? 屏幕切换至: %d", current_screen);
}

// =========================================================
// ? 核心状态机：指令分发
// =========================================================
static void process_ui_command(ui_cmd_t cmd) {
    switch (current_screen) {
        
        // ------------------------------------------------
        // 1. 在【AR 主界面】
        // ------------------------------------------------
        case SCREEN_MAIN_AR:
            if (cmd == UI_CMD_RIGHT) {
                switch_to_screen(SCREEN_MENU); // 右滑进入菜单
            }
            break;

        // ------------------------------------------------
        // 2. 在【主菜单】
        // ------------------------------------------------
        case SCREEN_MENU:
            if (cmd == UI_CMD_UP) {
                menu_scroll_up();
            }
            else if (cmd == UI_CMD_DOWN) {
                menu_scroll_down();
            }
            else if (cmd == UI_CMD_LEFT) {
                switch_to_screen(SCREEN_MAIN_AR); // 左滑退回主界面
            }
            else if (cmd == UI_CMD_RIGHT) {
                // 右滑确认，根据当前滚轮索引进相应的 APP
                uint16_t selected_idx = lv_roller_get_selected(menu_roller);
                if (selected_idx == 0) switch_to_screen(SCREEN_MAIN_AR);
                if (selected_idx == 2) switch_to_screen(SCREEN_CLOCK); // ? 选中第3项进时钟
                // 1 是健康，2 是 AI 对话 (预留)
                if (selected_idx == 3) switch_to_screen(SCREEN_NOVEL); // 进系统设置/小说
            }
            break;

        // ------------------------------------------------
        // 3. 在【小说阅读器】
        // ------------------------------------------------
        case SCREEN_NOVEL:
            if (cmd == UI_CMD_DOWN) {
                novel_scroll_one_line(); // 下滑读下一行
            }
            else if (cmd == UI_CMD_LEFT) {
                switch_to_screen(SCREEN_MENU); // 左滑退回菜单
            }
            break;

        // ------------------------------------------------
        // 4. 在【时钟工具】
        // ------------------------------------------------
        case SCREEN_CLOCK:
            clock_screen_handle_cmd(cmd); // ? 直接把手势交给时钟模块
            break;

        default:
            break;
    }
}

// =========================================================
// ? UI 守护任务：极度省电的设计
// =========================================================
static void ui_manager_task(void *pvParameter) {
    ui_cmd_t received_cmd;

    while (1) {
        // 死等队列消息 (portMAX_DELAY)。没有手势指令时，这个任务 0% CPU 占用！
        if (xQueueReceive(ui_cmd_queue, &received_cmd, portMAX_DELAY) == pdTRUE) {
            
            // 拿到官方锁，准备操作 UI
            if (lvgl_port_lock(0)) {
                process_ui_command(received_cmd);
                lvgl_port_unlock(); // 操作完释放锁
            }
        }
    }
}

// =========================================================
// ? 对外接口：一键初始化全套 UI
// =========================================================
void ui_manager_init(void) {
    // 1. 获取 LVGL 锁，集中初始化所有屏幕 (放进内存后台)
    if (lvgl_port_lock(0)) {
        ui_ar_glass_init();
        ui_menu_screen_init();
        ui_novel_screen_init();
        ui_clock_screen_init(); // ? 新增
        
        // 初始显示主屏幕
        lv_scr_load(ui_main_screen);
        lvgl_port_unlock();
    }

    // 2. 创建指令队列
    ui_cmd_queue = xQueueCreate(10, sizeof(ui_cmd_t));

    // 3. 启动 UI 守护任务
    xTaskCreatePinnedToCore(ui_manager_task, "ui_mgr", 1024 * 4, NULL, 5, NULL, 1);
    
    ESP_LOGI(TAG, "J.A.R.V.I.S UI 大管家启动完毕！");
}
