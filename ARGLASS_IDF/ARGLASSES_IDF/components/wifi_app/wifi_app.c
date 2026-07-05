/**
 * @file wifi_app.c
 * @brief WiFi STA (Station) 模式连接模块 (包含 NVS 记忆功能)
 */

#include "wifi_app.h"
#include <stdio.h>
#include <string.h>

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/* ==================== ESP-IDF 网络及系统头文件 ==================== */
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

/* =====================================================================
 * WiFi 配置参数 (默认回退参数)
 * ===================================================================== */
#define WIFI_SSID      "RUN"        // 默认 WiFi 热点名称
#define WIFI_PASS      "88888888"   // 默认 WiFi 密码
#define MAXIMUM_RETRY  3           // 最大重试次数

static const char *TAG = "WIFI_APP";
static int s_retry_num = 0;

/* =====================================================================
 * NVS 历史记录数据结构定义
 * ===================================================================== */
#define MAX_WIFI_RECORDS 10
#define NVS_NAMESPACE    "wifi_ns"
#define NVS_KEY_HISTORY  "wifi_history"

typedef struct {
    char ssid[32];
    char password[64];
} wifi_record_t;

typedef struct {
    uint8_t count;
    wifi_record_t records[MAX_WIFI_RECORDS];
} wifi_history_t;

// ====== 用于遍历历史 Wi-Fi 备用网络 ======
static wifi_record_t s_wifi_history[MAX_WIFI_RECORDS];
static int s_history_count = 0;
static int s_current_history_index = 0;

// ====== 终极兜底：定向扫描与挂起控制 ======
static bool s_fallback_scanning = false;    // 是否正在扫描默认网络
static bool s_fallback_connecting = false;  // 是否正在尝试连接默认网络
static bool s_give_up = false;              // 彻底放弃标志，为 true 时保持静默
// ==========================================


/* =====================================================================
 * 保存 WiFi 记录到 NVS（LRU 逻辑：最新使用的置顶，最多 10 条）
 * ===================================================================== */
void save_wifi_to_nvs(const char* ssid, const char* pwd) {
    nvs_handle_t handle;

    // 改用 malloc 在堆上分配，防止 sys_evt 任务栈溢出
    wifi_history_t *history = (wifi_history_t *)malloc(sizeof(wifi_history_t));
    if (history == NULL) {
        ESP_LOGE(TAG, "内存不足，无法保存 WiFi 记录");
        return;
    }
    memset(history, 0, sizeof(wifi_history_t));
    size_t required_size = sizeof(wifi_history_t);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        free(history);
        return;
    }

    // 1. 读取现有记录
    err = nvs_get_blob(handle, NVS_KEY_HISTORY, history, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "读取历史记录失败: %s", esp_err_to_name(err));
    }

    // 2. 查找是否已存在该 SSID
    int found_index = -1;
    for (int i = 0; i < history->count; i++) {
        if (strcmp(history->records[i].ssid, ssid) == 0) {
            found_index = i;
            break;
        }
    }

    // 3. 准备新记录
    wifi_record_t new_record = {0};
    strncpy(new_record.ssid, ssid, sizeof(new_record.ssid) - 1);
    strncpy(new_record.password, pwd, sizeof(new_record.password) - 1);

    // 4. 插入/更新并置顶
    if (found_index != -1) {
        for (int i = found_index; i > 0; i--) {
            history->records[i] = history->records[i - 1];
        }
        history->records[0] = new_record;
        ESP_LOGI(TAG, "💾 更新 WiFi [%s] 并置顶", ssid);
    } else {
        int shift = (history->count < MAX_WIFI_RECORDS) ? history->count : (MAX_WIFI_RECORDS - 1);
        for (int i = shift; i > 0; i--) {
            history->records[i] = history->records[i - 1];
        }
        history->records[0] = new_record;
        if (history->count < MAX_WIFI_RECORDS) history->count++;
        ESP_LOGI(TAG, "💾 新增 WiFi [%s]，共 %d 条记录", ssid, history->count);
    }

    // 5. 保存回 NVS
    err = nvs_set_blob(handle, NVS_KEY_HISTORY, history, sizeof(wifi_history_t));
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);

    free(history);  // 释放堆内存
}

/* =====================================================================
 * 从 NVS 读取 WiFi 历史记录
 * ===================================================================== */
int load_wifi_history(wifi_record_t *out_records, int max_count) {
    nvs_handle_t handle;

    // 改用 malloc 在堆上分配，防止 sys_evt 任务栈溢出
    wifi_history_t *history = (wifi_history_t *)malloc(sizeof(wifi_history_t));
    if (history == NULL) return 0;
    memset(history, 0, sizeof(wifi_history_t));
    size_t required_size = sizeof(wifi_history_t);

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        free(history);
        return 0;
    }
    if (nvs_get_blob(handle, NVS_KEY_HISTORY, history, &required_size) != ESP_OK) {
        nvs_close(handle);
        free(history);
        return 0;
    }
    nvs_close(handle);

    int copy_count = (history->count < max_count) ? history->count : max_count;
    memcpy(out_records, history->records, copy_count * sizeof(wifi_record_t));

    free(history);  // 释放堆内存
    return copy_count;
}

/* =====================================================================
 * WiFi 事件回调函数 (定向扫描 + 备用轮询 + 彻底挂起)
 * =====================================================================
 * 状态机：
 *   1. 首选网络重试 10 次 → 失败
 *   2. 轮询备用历史网络 (跳过空 SSID) → 全部失败
 *   3. 定向扫描默认网络 (异步不阻塞) → 找到则连一次，找不到则挂起
 *   4. 挂起状态 (s_give_up)：所有 DISCONNECTED 事件直接 return
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {

    // --- 1. 启动事件 ---
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_give_up = false;
        s_fallback_scanning = false;
        s_fallback_connecting = false;
        esp_wifi_connect();
    }

    // --- 2. 扫描完成事件 (处理兜底扫描结果) ---
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (s_fallback_scanning) {
            s_fallback_scanning = false;

            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);

            if (ap_count > 0) {
                ESP_LOGI(TAG, "🔍 扫描发现默认网络 [%s]，发起最后一次连接...", WIFI_SSID);
                s_fallback_connecting = true;

                wifi_config_t wifi_config = {0};
                strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
                strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

                esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "❌ 扫描未发现默认网络 [%s]，彻底放弃！", WIFI_SSID);
                s_give_up = true;
            }
        }
    }

    // --- 3. 断开连接事件 ---
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_give_up) return;  // 已放弃，绝对静默

        if (s_fallback_connecting) {
            // 兜底默认网络连接失败 → 彻底放弃
            ESP_LOGE(TAG, "❌ 默认网络 [%s] 连接失败，彻底放弃！", WIFI_SSID);
            s_give_up = true;
            return;
        }

        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi 断开，第 %d 次重连...", s_retry_num);
        } else {
            // 当前网络失败，寻找下一个有效历史 WiFi
            s_current_history_index++;

            // 跳过空 SSID 的坏数据
            while (s_current_history_index < s_history_count &&
                   strlen(s_wifi_history[s_current_history_index].ssid) == 0) {
                s_current_history_index++;
            }

            if (s_current_history_index < s_history_count) {
                ESP_LOGW(TAG, "首选失败，尝试备用: [%s]", s_wifi_history[s_current_history_index].ssid);

                wifi_config_t wifi_config = {0};
                strncpy((char *)wifi_config.sta.ssid, s_wifi_history[s_current_history_index].ssid, sizeof(wifi_config.sta.ssid) - 1);
                strncpy((char *)wifi_config.sta.password, s_wifi_history[s_current_history_index].password, sizeof(wifi_config.sta.password) - 1);
                wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

                esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                esp_wifi_connect();
                s_retry_num = 0;
            } else {
                // 历史记录全军覆没 → 异步定向扫描默认网络
                ESP_LOGE(TAG, "所有历史 WiFi 无效！扫描默认网络 [%s]...", WIFI_SSID);
                s_retry_num = 0;
                s_history_count = 0;

                s_fallback_scanning = true;
                wifi_scan_config_t scan_cfg = {0};
                scan_cfg.ssid = (uint8_t *)WIFI_SSID;  // 只找指定 SSID
                esp_wifi_scan_start(&scan_cfg, false);  // 异步扫描，不阻塞
            }
        }
    }

    // --- 4. 获取到 IP ---
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ 成功连网! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // 备用网络连上了 → 置顶到 NVS
        if (s_current_history_index > 0 && s_current_history_index < s_history_count && !s_fallback_connecting) {
            save_wifi_to_nvs(s_wifi_history[s_current_history_index].ssid,
                             s_wifi_history[s_current_history_index].password);
            s_history_count = load_wifi_history(s_wifi_history, MAX_WIFI_RECORDS);
        }

        // 清除所有异常标志
        s_retry_num = 0;
        s_current_history_index = 0;
        s_give_up = false;
        s_fallback_scanning = false;
        s_fallback_connecting = false;
    }
}

/* =====================================================================
 * WiFi 初始化函数 (STA 模式)
 * ===================================================================== */
void wifi_init_sta(void) {
    // 1. 初始化 NVS 闪存 (Wi-Fi 模块和历史记录都依赖它)
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
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // 2. 从 NVS 读取所有 WiFi 历史记录 (支持备用网络自动轮询)
    s_history_count = load_wifi_history(s_wifi_history, MAX_WIFI_RECORDS);
    s_current_history_index = 0;

    // 增加 strlen 校验，防止读取到崩溃导致的脏数据 (空 SSID)
    if (s_history_count > 0 && strlen(s_wifi_history[0].ssid) > 0) {
        strncpy((char *)wifi_config.sta.ssid, s_wifi_history[0].ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, s_wifi_history[0].password, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "🔍 加载首选 Wi-Fi -> SSID: [%s] (共 %d 条历史)", s_wifi_history[0].ssid, s_history_count);
    } else {
        // NVS 为空或数据损坏，使用默认配置
        strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
        ESP_LOGI(TAG, "⚠️ 无有效历史记录，使用默认 Wi-Fi -> SSID: [%s]", WIFI_SSID);
        s_history_count = 0;  // 数据坏了，清零防止轮询到坏数据
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished. Waiting for events...");
}

/* =====================================================================
 * 动态连接 Wi-Fi (供 UART 串口配网调用)
 * ===================================================================== */
void wifi_connect_dynamic(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "🔄 收到串口配网指令，准备连接新 Wi-Fi: %s", ssid);

    // 1. 将新收到的账号密码保存到 NVS，防止重启后丢失
    save_wifi_to_nvs(ssid, password);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    // 重置所有状态，解除挂起
    s_retry_num = 0;
    s_current_history_index = 0;
    s_give_up = false;
    s_fallback_scanning = false;
    s_fallback_connecting = false;
}