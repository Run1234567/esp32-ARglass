#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "my_wifi.h"

// ============================================================
//   WiFi 历史记录（NVS 存储，最多 10 条，LRU 置顶）
// ============================================================
#define MAX_WIFI_RECORDS 10
#define NVS_NAMESPACE    "wifi_ns"
#define NVS_KEY_HISTORY  "wifi_history"

typedef struct {
    uint8_t count;
    wifi_record_t records[MAX_WIFI_RECORDS];
} wifi_history_t;

// ================== 配置区 ==================
#define WIFI_SSID      "RUN"
#define WIFI_PASS      "88888888"
#define MAXIMUM_RETRY  5

static const char *TAG = "WIFI_COMP";
static int s_retry_num = 0;

// 事件回调函数 (内部使用，不暴露给外部)
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi断开，第 %d 次重连...", s_retry_num);
        } else {
            ESP_LOGE(TAG, "Wi-Fi连接失败，已放弃。");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ 成功连网! IP地址: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

// 这是暴露给 main.c 调用的核心初始化函数
void wifi_init_sta(void) {
    // 1. 初始化 NVS 闪存 (Wi-Fi 驱动的硬性要求)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化底层网络堆栈
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. 配置并启动 Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&cfg);
    if (wifi_err != ESP_OK) {
        ESP_LOGW("WIFI", "WiFi 初始化失败 (0x%x)，内存不足，跳过 WiFi", wifi_err);
        return;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi 模块初始化完毕，正在后台连接...");
}

// ============================================================
//   保存 WiFi 记录到 NVS（LRU 置顶，最多 10 条）
// ============================================================
void save_wifi_to_nvs(const char* ssid, const char* pwd) {
    nvs_handle_t handle;
    wifi_history_t history = {0};
    size_t required_size = sizeof(wifi_history_t);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return;
    }

    // 读取现有记录
    err = nvs_get_blob(handle, NVS_KEY_HISTORY, &history, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "读取历史记录失败: %s", esp_err_to_name(err));
    }

    // 查找是否已存在
    int found_index = -1;
    for (int i = 0; i < history.count; i++) {
        if (strcmp(history.records[i].ssid, ssid) == 0) {
            found_index = i;
            break;
        }
    }

    // 准备新记录
    wifi_record_t new_record = {0};
    strncpy(new_record.ssid, ssid, sizeof(new_record.ssid) - 1);
    strncpy(new_record.password, pwd, sizeof(new_record.password) - 1);

    // 插入/更新并置顶
    if (found_index != -1) {
        // 已存在：更新密码，移到最前
        for (int i = found_index; i > 0; i--) {
            history.records[i] = history.records[i - 1];
        }
        history.records[0] = new_record;
        ESP_LOGI(TAG, "更新 WiFi [%s] 并置顶", ssid);
    } else {
        // 新记录：插入最前，挤掉最后一个
        int shift = (history.count < MAX_WIFI_RECORDS) ? history.count : (MAX_WIFI_RECORDS - 1);
        for (int i = shift; i > 0; i--) {
            history.records[i] = history.records[i - 1];
        }
        history.records[0] = new_record;
        if (history.count < MAX_WIFI_RECORDS) history.count++;
        ESP_LOGI(TAG, "新增 WiFi [%s]，共 %d 条记录", ssid, history.count);
    }

    // 保存回 NVS
    err = nvs_set_blob(handle, NVS_KEY_HISTORY, &history, sizeof(wifi_history_t));
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

// ============================================================
//   从 NVS 读取 WiFi 历史记录
// ============================================================
int load_wifi_history(wifi_record_t *out_records, int max_count) {
    nvs_handle_t handle;
    wifi_history_t history = {0};
    size_t required_size = sizeof(wifi_history_t);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
    if (nvs_get_blob(handle, NVS_KEY_HISTORY, &history, &required_size) != ESP_OK) {
        nvs_close(handle);
        return 0;
    }
    nvs_close(handle);

    int copy_count = (history.count < max_count) ? history.count : max_count;
    memcpy(out_records, history.records, copy_count * sizeof(wifi_record_t));
    return copy_count;
}

// ============================================================
//   供 BLE 配网调用的连接函数
// ============================================================
void my_wifi_connect_from_ble(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "BLE 配网: 连接到 %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    s_retry_num = 0; // 重置重试计数
}