/**
 * @file ui_wifi_scan_screen.c
 * @brief Wi-Fi 扫描界面 —— 黑底白字风格
 */

#include "ui_wifi_scan_screen.h"
#include "ui_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "UI_WIFI_SCAN";

lv_obj_t * ui_wifi_scan_screen;
static lv_obj_t * ui_wifi_list;
static lv_obj_t * label_scan_status;

// ============================================================
//   后台扫描任务
// ============================================================
static void wifi_scan_task(void *pvParameters) {
    ESP_LOGI(TAG, "扫描任务启动");

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 未初始化");
        if (lvgl_port_lock(-1)) {
            lv_label_set_text(label_scan_status, "Wi-Fi 未初始化");
            lvgl_port_unlock();
        }
        vTaskDelete(NULL);
        return;
    }

    wifi_scan_config_t scan_config = {
        .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = false
    };

    // 非阻塞扫描，避免被 MQTT/WebSocket 卡住
    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    ESP_LOGI(TAG, "扫描启动: %s", esp_err_to_name(err));

    // 等待 10 秒让扫描完成
    vTaskDelay(pdMS_TO_TICKS(10000));

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "AP 数量: %d", ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_info != NULL) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_info);

            // 合并同名 Wi-Fi，保留信号最强的一个
            wifi_ap_record_t *unique_aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            int unique_count = 0;

            if (unique_aps != NULL) {
                for (int i = 0; i < ap_count; i++) {
                    if (strlen((char *)ap_info[i].ssid) == 0) continue;

                    bool is_duplicate = false;
                    for (int j = 0; j < unique_count; j++) {
                        if (strcmp((char *)ap_info[i].ssid, (char *)unique_aps[j].ssid) == 0) {
                            is_duplicate = true;
                            if (ap_info[i].rssi > unique_aps[j].rssi) {
                                unique_aps[j] = ap_info[i];
                            }
                            break;
                        }
                    }
                    if (!is_duplicate) {
                        unique_aps[unique_count] = ap_info[i];
                        unique_count++;
                    }
                }

                ESP_LOGI(TAG, "去重后 %d 个独立 Wi-Fi", unique_count);

                if (lvgl_port_lock(-1)) {
                    lv_label_set_text_fmt(label_scan_status, "发现 %d 个网络", unique_count);
                    lv_obj_clean(ui_wifi_list);

                    for (int i = 0; i < unique_count; i++) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "%s (%d)", unique_aps[i].ssid, unique_aps[i].rssi);

                        lv_obj_t * item = lv_list_add_text(ui_wifi_list, buf);
                        lv_obj_set_style_text_color(item, lv_color_white(), 0);
                        lv_obj_set_style_bg_color(item, lv_color_black(), 0);
                        lv_obj_set_style_border_width(item, 0, 0);
                    }

                    lvgl_port_unlock();
                }

                free(unique_aps);
            }

            free(ap_info);
        }
    } else {
        if (lvgl_port_lock(-1)) {
            lv_label_set_text(label_scan_status, "未发现网络");
            lvgl_port_unlock();
        }
    }

    ESP_LOGI(TAG, "扫描结束");
    vTaskDelete(NULL);
}

// ============================================================
//   触发扫描
// ============================================================
void ui_wifi_scan_start(void) {
    ESP_LOGI(TAG, "触发扫描");
    if (lvgl_port_lock(-1)) {
        lv_label_set_text(label_scan_status, "正在扫描...");
        lv_obj_clean(ui_wifi_list);
        lvgl_port_unlock();
    }

    BaseType_t ret = xTaskCreatePinnedToCore(wifi_scan_task, "wifi_scan", 3072, NULL, 5, NULL, tskNO_AFFINITY);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "扫描任务创建失败！内存不足");
        if (lvgl_port_lock(-1)) {
            lv_label_set_text(label_scan_status, "系统内存不足");
            lvgl_port_unlock();
        }
    }
}

// ============================================================
//   界面初始化
// ============================================================
void ui_wifi_scan_screen_init(void) {
    ui_wifi_scan_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_wifi_scan_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_wifi_scan_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wifi_scan_screen, 0, 0);
    lv_obj_set_style_radius(ui_wifi_scan_screen, 0, 0);

    // 顶部状态标签
    label_scan_status = lv_label_create(ui_wifi_scan_screen);
    lv_obj_set_style_text_font(label_scan_status, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_scan_status, lv_color_white(), 0);
    lv_label_set_text(label_scan_status, "准备扫描...");
    lv_obj_align(label_scan_status, LV_ALIGN_TOP_MID, 0, 10);

    // Wi-Fi 列表
    ui_wifi_list = lv_list_create(ui_wifi_scan_screen);
    lv_obj_set_size(ui_wifi_list, 220, 180);
    lv_obj_align(ui_wifi_list, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 彻底清除列表默认主题样式
    lv_obj_set_style_bg_color(ui_wifi_list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_wifi_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_wifi_list, 0, 0);
    lv_obj_set_style_radius(ui_wifi_list, 0, 0);
    lv_obj_set_style_text_font(ui_wifi_list, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(ui_wifi_list, lv_color_white(), 0);

    // 隐藏滚动条（防止滑动时出现白条）
    lv_obj_set_style_opa(ui_wifi_list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
}

// ============================================================
//   手势处理
// ============================================================
void wifi_scan_screen_handle_cmd(ui_cmd_t cmd) {
    if (cmd == UI_CMD_LEFT || cmd == UI_CMD_BACKWARD || cmd == UI_CMD_CIRCLE) {
        extern void switch_to_screen(ui_screen_state_t target);
        switch_to_screen(SCREEN_MENU);
    }
}
