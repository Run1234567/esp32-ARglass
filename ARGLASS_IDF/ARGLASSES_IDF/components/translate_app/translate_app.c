/**
 * @file translate_app.c
 * @brief 火山引擎同传 WebSocket 客户端 (极速低延迟版)
 *
 * 通过 WebSocket 将麦克风 PCM 音频实时发送到云端，
 * 云端转发给火山引擎翻译并返回结果。
 *
 * 优势：无 HTTP 开销，真正的双向实时通信。
 */

#include "translate_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

// 避免循环依赖，直接声明外部函数
extern void my_uart_send(const char *data);
extern void playSpeaker(const uint8_t *data, size_t len);
extern SemaphoreHandle_t speaker_mutex;

// esp_event 是 esp_websocket_client 的私有依赖，手动定义类型绕过
typedef const char* esp_event_base_t;

static const char *TAG = "translate_ws";

// --- 配置 ---
#define CHUNK_SIZE             3200
#define TRANSLATE_TASK_STACK   8192
#define TRANSLATE_TASK_CORE    0
#define TRANSLATE_TASK_PRIO    4

#define DEFAULT_SERVER_IP      "124.220.224.189"
#define DEFAULT_SERVER_PORT    5001

// --- 状态 ---
static volatile bool s_active = false;
static char s_server_url[128] = {0};
static char s_src_lang[16] = "en";
static char s_tgt_lang[16] = "zh";
static char s_mode[16] = "s2s";

// --- 句柄 ---
static RingbufHandle_t s_ringbuf = NULL;
static TaskHandle_t s_task_handle = NULL;
static esp_websocket_client_handle_t s_ws_client = NULL;

// ============================================================
//  WebSocket 事件回调
// ============================================================
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "🟢 WebSocket 已连接！");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "🔴 WebSocket 已断开！");
            break;

        case WEBSOCKET_EVENT_DATA:
            // 1. 文本帧: 翻译结果
            if (data->op_code == 0x01 && data->data_len > 0) {
                char *json_str = malloc(data->data_len + 1);
                if (json_str) {
                    memcpy(json_str, data->data_ptr, data->data_len);
                    json_str[data->data_len] = '\0';

                    cJSON *root = cJSON_Parse(json_str);
                    if (root) {
                        cJSON *type = cJSON_GetObjectItem(root, "type");
                        cJSON *text = cJSON_GetObjectItem(root, "text");

                        if (cJSON_IsString(type) && cJSON_IsString(text)) {
                            if (strcmp(type->valuestring, "source") == 0) {
                                ESP_LOGI(TAG, "📝 原文: %s", text->valuestring);
                                char uart_buf[512];
                                // 日语/韩语留白（UI MCU 无法显示）
                                if (strncmp(s_src_lang, "ja", 2) == 0 || strncmp(s_src_lang, "jp", 2) == 0 ||
                                    strncmp(s_src_lang, "ko", 2) == 0 || strncmp(s_src_lang, "kr", 2) == 0) {
                                    snprintf(uart_buf, sizeof(uart_buf), "TRS:\r\n");
                                } else {
                                    snprintf(uart_buf, sizeof(uart_buf), "TRS:%s\r\n", text->valuestring);
                                }
                                my_uart_send(uart_buf);
                            }
                            else if (strcmp(type->valuestring, "translation") == 0) {
                                ESP_LOGI(TAG, "🌐 翻译: %s", text->valuestring);
                                char uart_buf[512];
                                // 日语/韩语留白
                                if (strncmp(s_tgt_lang, "ja", 2) == 0 || strncmp(s_tgt_lang, "jp", 2) == 0 ||
                                    strncmp(s_tgt_lang, "ko", 2) == 0 || strncmp(s_tgt_lang, "kr", 2) == 0) {
                                    snprintf(uart_buf, sizeof(uart_buf), "TRT:\r\n");
                                } else {
                                    snprintf(uart_buf, sizeof(uart_buf), "TRT:%s\r\n", text->valuestring);
                                }
                                my_uart_send(uart_buf);
                            }
                            else if (strcmp(type->valuestring, "error") == 0) {
                                ESP_LOGE(TAG, "❌ 云端报错: %s", text->valuestring);
                            }
                        }
                        cJSON_Delete(root);
                    }
                    free(json_str);
                }
            }
            // 2. 二进制帧: 云端下发的 PCM 音频
            else if (data->op_code == 0x02 && data->data_len > 0) {
                if (speaker_mutex != NULL) {
                    if (xSemaphoreTake(speaker_mutex, portMAX_DELAY) == pdTRUE) {
                        playSpeaker((const uint8_t *)data->data_ptr, data->data_len);
                        xSemaphoreGive(speaker_mutex);
                    }
                } else {
                    playSpeaker((const uint8_t *)data->data_ptr, data->data_len);
                }
            }
            break;
    }
}

// ============================================================
//  主任务
// ============================================================
static void translate_task(void *arg)
{
    ESP_LOGI(TAG, "Translate WebSocket task started");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_active) continue;

        // 清空残留音频
        size_t flush_size = 0;
        void *flush_data = NULL;
        while ((flush_data = xRingbufferReceive(s_ringbuf, &flush_size, 0)) != NULL) {
            vRingbufferReturnItem(s_ringbuf, flush_data);
        }

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  🌐 连接: %s ...", s_server_url);
        ESP_LOGI(TAG, "========================================");

        esp_websocket_client_config_t ws_cfg = {
            .uri = s_server_url,
            .network_timeout_ms = 10000,
        };

        s_ws_client = esp_websocket_client_init(&ws_cfg);
        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                      websocket_event_handler, (void *)s_ws_client);
        esp_websocket_client_start(s_ws_client);

        int wait_ms = 0;
        while (!esp_websocket_client_is_connected(s_ws_client) && wait_ms < 5000 && s_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }

        if (!esp_websocket_client_is_connected(s_ws_client)) {
            ESP_LOGE(TAG, "❌ 连接超时！");
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
            s_active = false;
            continue;
        }

        // 发送 Start 指令
        char start_cmd[128];
        snprintf(start_cmd, sizeof(start_cmd),
                 "{\"action\": \"start\", \"source_lang\": \"%s\", \"target_lang\": \"%s\", \"mode\": \"%s\"}",
                 s_src_lang, s_tgt_lang, s_mode);
        esp_websocket_client_send_text(s_ws_client, start_cmd, strlen(start_cmd), portMAX_DELAY);

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  ✅ 请开始说话...");
        ESP_LOGI(TAG, "========================================");

        // 音频推流
        size_t accum_len = 0;
        uint8_t accum_buf[CHUNK_SIZE];

        while (s_active && esp_websocket_client_is_connected(s_ws_client)) {
            size_t item_size = 0;
            uint8_t *data = (uint8_t *)xRingbufferReceive(s_ringbuf, &item_size, pdMS_TO_TICKS(100));

            if (data != NULL && item_size > 0) {
                if (accum_len + item_size > CHUNK_SIZE && accum_len > 0) {
                    esp_websocket_client_send_bin(s_ws_client, (const char *)accum_buf,
                                                  accum_len, portMAX_DELAY);
                    accum_len = 0;
                }

                if (item_size >= CHUNK_SIZE) {
                    esp_websocket_client_send_bin(s_ws_client, (const char *)data,
                                                  item_size, portMAX_DELAY);
                } else {
                    memcpy(accum_buf + accum_len, data, item_size);
                    accum_len += item_size;
                }

                vRingbufferReturnItem(s_ringbuf, data);
            }
        }

        // 清理
        if (accum_len > 0) {
            esp_websocket_client_send_bin(s_ws_client, (const char *)accum_buf,
                                          accum_len, portMAX_DELAY);
            accum_len = 0;
        }

        while (esp_websocket_client_is_connected(s_ws_client)) {
            size_t left_size = 0;
            uint8_t *left_data = (uint8_t *)xRingbufferReceive(s_ringbuf, &left_size, 0);
            if (left_data == NULL) break;
            esp_websocket_client_send_bin(s_ws_client, (const char *)left_data,
                                          left_size, portMAX_DELAY);
            vRingbufferReturnItem(s_ringbuf, left_data);
        }

        if (esp_websocket_client_is_connected(s_ws_client)) {
            const char *stop_cmd = "{\"action\": \"stop\"}";
            esp_websocket_client_send_text(s_ws_client, stop_cmd, strlen(stop_cmd), portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  ⏹️ 翻译已停止");
        ESP_LOGI(TAG, "========================================");
    }
}

// ============================================================
//  公共 API
// ============================================================

void translate_app_init(RingbufHandle_t rb)
{
    if (rb == NULL) {
        ESP_LOGE(TAG, "初始化失败！");
        return;
    }
    s_ringbuf = rb;

    BaseType_t ret = xTaskCreatePinnedToCore(
        translate_task, "translate_ws_task", TRANSLATE_TASK_STACK,
        NULL, TRANSLATE_TASK_PRIO, &s_task_handle, TRANSLATE_TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_ringbuf = NULL;
        return;
    }
    ESP_LOGI(TAG, "✅ translate_app (WebSocket) 已初始化");
}

void translate_start(const char *server_ip, int port)
{
    if (s_active) {
        ESP_LOGW(TAG, "⚠️ 翻译已在运行中");
        return;
    }

    const char *ip = (server_ip && server_ip[0]) ? server_ip : DEFAULT_SERVER_IP;
    int p = (port > 0) ? port : DEFAULT_SERVER_PORT;

    snprintf(s_server_url, sizeof(s_server_url), "ws://%s:%d", ip, p);
    s_active = true;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  🌐 翻译启动 → %s", s_server_url);
    ESP_LOGI(TAG, "========================================");

    if (s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

void translate_stop(void)
{
    if (!s_active) {
        ESP_LOGW(TAG, "⚠️ 翻译未在运行");
        return;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ⏹️ 翻译停止");
    ESP_LOGI(TAG, "========================================");

    s_active = false;
}

bool translate_is_active(void)
{
    return s_active;
}

void translate_set_language(const char *src, const char *tgt)
{
    if (src && strlen(src) > 0) {
        strncpy(s_src_lang, src, sizeof(s_src_lang) - 1);
        s_src_lang[sizeof(s_src_lang) - 1] = '\0';
    }
    if (tgt && strlen(tgt) > 0) {
        strncpy(s_tgt_lang, tgt, sizeof(s_tgt_lang) - 1);
        s_tgt_lang[sizeof(s_tgt_lang) - 1] = '\0';
    }
    ESP_LOGI(TAG, "语言: %s → %s", s_src_lang, s_tgt_lang);
}

void translate_set_mode(const char *mode)
{
    if (mode && strlen(mode) > 0) {
        strncpy(s_mode, mode, sizeof(s_mode) - 1);
        s_mode[sizeof(s_mode) - 1] = '\0';
        ESP_LOGI(TAG, "模式: %s", s_mode);
    }
}
