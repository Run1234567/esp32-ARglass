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
#define MAXIMUM_RETRY  3// 最大重连次数

static const char *TAG = "WIFI_COMP";
static int s_retry_num = 0;

// 用于在内存中遍历历史 Wi-Fi
static wifi_record_t s_wifi_history[MAX_WIFI_RECORDS];
static int s_history_count = 0;
static int s_current_history_index = 0;

// ============================================================
//   保存 WiFi 记录到 NVS（LRU 置顶，最多 10 条）
// ============================================================
void save_wifi_to_nvs(const char* ssid, const char* pwd) {
    nvs_handle_t handle;
    wifi_history_t *h = malloc(sizeof(wifi_history_t));
    if (!h) { ESP_LOGE(TAG, "malloc 失败"); return; }
    memset(h, 0, sizeof(wifi_history_t));
    size_t required_size = sizeof(wifi_history_t);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        free(h);
        return;
    }

    nvs_get_blob(handle, NVS_KEY_HISTORY, h, &required_size);

    int found_index = -1;
    for (int i = 0; i < h->count; i++) {
        if (strcmp(h->records[i].ssid, ssid) == 0) {
            found_index = i;
            break;
        }
    }

    wifi_record_t new_record = {0};
    strncpy(new_record.ssid, ssid, sizeof(new_record.ssid) - 1);
    strncpy(new_record.password, pwd, sizeof(new_record.password) - 1);

    if (found_index != -1) {
        for (int i = found_index; i > 0; i--)
            h->records[i] = h->records[i - 1];
        h->records[0] = new_record;
        ESP_LOGI(TAG, "更新 WiFi [%s] 并置顶", ssid);
    } else {
        int shift = (h->count < MAX_WIFI_RECORDS) ? h->count : (MAX_WIFI_RECORDS - 1);
        for (int i = shift; i > 0; i--)
            h->records[i] = h->records[i - 1];
        h->records[0] = new_record;
        if (h->count < MAX_WIFI_RECORDS) h->count++;
        ESP_LOGI(TAG, "新增 WiFi [%s]，共 %d 条", ssid, h->count);
    }

    err = nvs_set_blob(handle, NVS_KEY_HISTORY, h, sizeof(wifi_history_t));
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
    free(h);
}

// ============================================================
//   从 NVS 读取 WiFi 历史记录
// ============================================================
int load_wifi_history(wifi_record_t *out_records, int max_count) {
    nvs_handle_t handle;
    wifi_history_t *history = malloc(sizeof(wifi_history_t));
    if (!history) return 0;
    memset(history, 0, sizeof(wifi_history_t));
    size_t required_size = sizeof(wifi_history_t);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) { free(history); return 0; }
    if (nvs_get_blob(handle, NVS_KEY_HISTORY, history, &required_size) != ESP_OK) {
        nvs_close(handle);
        free(history);
        return 0;
    }
    nvs_close(handle);

    int copy_count = (history->count < max_count) ? history->count : max_count;
    memcpy(out_records, history->records, copy_count * sizeof(wifi_record_t));
    free(history);
    return copy_count;
}

// ============================================================
//   事件回调函数（断网时自动遍历备用网络）
// ============================================================
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi断开，第 %d 次重连...", s_retry_num);
        } else {
            // 当前网络失败，尝试下一个历史 WiFi
            s_current_history_index++;
            if (s_current_history_index < s_history_count) {
                ESP_LOGW(TAG, "首选网络失败，尝试备用: [%s]", s_wifi_history[s_current_history_index].ssid);

                wifi_config_t wifi_config = {0};
                strncpy((char *)wifi_config.sta.ssid, s_wifi_history[s_current_history_index].ssid, sizeof(wifi_config.sta.ssid) - 1);
                strncpy((char *)wifi_config.sta.password, s_wifi_history[s_current_history_index].password, sizeof(wifi_config.sta.password) - 1);
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

                esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                esp_wifi_connect();
                s_retry_num = 0;
            } else {
                ESP_LOGE(TAG, "所有 %d 个历史 WiFi 均连接失败！", s_history_count);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "成功连网! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // 网络就绪，错峰启动 MQTT 和 WebSocket（避免同时吃光内存）
        ESP_LOGI(TAG, "启动云端通信...");
        extern void app_mqtt_start(void);
        app_mqtt_start();

        vTaskDelay(pdMS_TO_TICKS(3000)); // 等 MQTT 握手完成，内存释放后再启 WebSocket

        typedef void* ws_handle_t;
        extern ws_handle_t ws_client;
        extern esp_err_t esp_websocket_client_start(ws_handle_t client);
        if (ws_client != NULL) {
            esp_websocket_client_start(ws_client);
        }

        // 备用网络连上了 → 置顶到 NVS
        if (s_current_history_index > 0 && s_current_history_index < s_history_count) {
            save_wifi_to_nvs(s_wifi_history[s_current_history_index].ssid,
                             s_wifi_history[s_current_history_index].password);
            s_history_count = load_wifi_history(s_wifi_history, MAX_WIFI_RECORDS);
        }

        s_retry_num = 0;
        s_current_history_index = 0;
    }
}

// ============================================================
//   核心初始化函数
// ============================================================
void wifi_init_sta(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_err = esp_wifi_init(&cfg);
    if (wifi_err != ESP_OK) {
        ESP_LOGW("WIFI", "WiFi 初始化失败 (0x%x)", wifi_err);
        return;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    // 加载所有历史记录到内存
    s_history_count = load_wifi_history(s_wifi_history, MAX_WIFI_RECORDS);
    s_current_history_index = 0;

    if (s_history_count > 0) {
        strncpy((char *)wifi_config.sta.ssid, s_wifi_history[0].ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, s_wifi_history[0].password, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "从 NVS 加载首选 WiFi: %s", s_wifi_history[0].ssid);
    } else {
        strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "无历史记录，使用默认: %s", WIFI_SSID);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi 模块初始化完毕，正在后台连接...");
}

// ============================================================
//   供 BLE / UART 配网调用的连接函数
// ============================================================
void my_wifi_connect_from_ble(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "外部配网: 连接到 %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    s_retry_num = 0;
    s_current_history_index = 0;
}
