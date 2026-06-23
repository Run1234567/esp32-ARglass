/**
 * @file ai_chat_ws.c
 * @brief WebSocket 客户端：OTA 发现 + WebSocket 连接
 *
 * 连接流程（参照 digital-human 项目 ota-connector.js）：
 *   1. HTTP POST OTA 端点 → 获取 WebSocket URL 和 token
 *   2. 建立 WebSocket 连接，URL 附加 authorization/device-id/client-id
 *   3. 事件回调分发 text/binary 消息
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "ai_chat_ws.h"

static const char *TAG = "AI_WS";

/* ==================== 服务器配置 ==================== */
#define OTA_URL         "http://1.12.46.115:8002/xiaozhi/ota/"
#define DEVICE_NAME     "ARGLASS-JARVIS"
#define APP_VERSION     "1.0.0"

/* ==================== 静态变量 ==================== */
static esp_websocket_client_handle_t s_ws_client = NULL;
static ws_msg_cb_t s_msg_cb = NULL;
static ws_state_cb_t s_state_cb = NULL;

/* OTA 返回的信息 */
static char s_ws_url[512] = {0};      // 完整的 WebSocket URL（含参数）
static char s_ws_token[256] = {0};    // 认证 token

/* 设备 MAC 地址字符串 */
static char s_device_mac[18] = {0};

/**
 * @brief 获取设备 MAC 地址字符串
 */
static void get_mac_str(void) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_device_mac, sizeof(s_device_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ==================== OTA 发现 ==================== */

int ai_chat_ota_discover(void) {
    get_mac_str();
    ESP_LOGI(TAG, "正在 OTA 发现... MAC=%s", s_device_mac);

    /* 构造 OTA 请求 JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 0);
    cJSON_AddStringToObject(root, "uuid", "");

    cJSON *app = cJSON_CreateObject();
    cJSON_AddStringToObject(app, "name", "xiaozhi-esp32s3");
    cJSON_AddStringToObject(app, "version", APP_VERSION);
    cJSON_AddStringToObject(app, "compile_time", __DATE__ " " __TIME__);
    cJSON_AddStringToObject(app, "idf_version", "5.5.0");
    cJSON_AddStringToObject(app, "elf_sha256", "0000000000000000");
    cJSON_AddItemToObject(root, "application", app);

    cJSON *board = cJSON_CreateObject();
    cJSON_AddStringToObject(board, "type", DEVICE_NAME);
    cJSON_AddStringToObject(board, "ssid", "ARGLASS");
    cJSON_AddNumberToObject(board, "rssi", 0);
    cJSON_AddNumberToObject(board, "channel", 0);
    cJSON_AddStringToObject(board, "ip", "0.0.0.0");
    cJSON_AddStringToObject(board, "mac", s_device_mac);
    cJSON_AddItemToObject(root, "board", board);

    cJSON_AddNumberToObject(root, "flash_size", 8 * 1024 * 1024);
    cJSON_AddStringToObject(root, "mac_address", s_device_mac);

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    /* 发送 HTTP POST — 硬核分步读取 */
    esp_http_client_config_t config = {
        .url = OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", s_device_mac);
    esp_http_client_set_header(client, "Client-Id", s_device_mac);

    /* 1. 打开连接并准备发送数据 */
    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 打开连接失败: %s", esp_err_to_name(err));
        free(post_data);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 2. 写入 POST 数据给服务器 */
    int write_len = esp_http_client_write(client, post_data, strlen(post_data));
    free(post_data);
    if (write_len < 0) {
        ESP_LOGE(TAG, "OTA 写入数据失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 3. 抓取响应头（拿到真实的 Content-Length） */
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OTA 状态码: %d, Content-Length: %d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "OTA 返回状态码: %d", status);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 4. 根据服务器返回的长度精确分配内存 */
    if (content_length <= 0) {
        content_length = 512;
    }

    char *buf = malloc(content_length + 1);
    if (!buf) {
        ESP_LOGE(TAG, "内存分配失败");
        esp_http_client_cleanup(client);
        return -1;
    }

    /* 5. 直读响应体 */
    int read_len = esp_http_client_read(client, buf, content_length);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "读取响应失败, read_len=%d", read_len);
        free(buf);
        esp_http_client_cleanup(client);
        return -1;
    }

    buf[read_len] = '\0';
    ESP_LOGI(TAG, "OTA 截获数据! 长度: %d", read_len);
    ESP_LOGI(TAG, "OTA 响应: %s", buf);

    /* 解析 JSON 响应 */
    cJSON *resp = cJSON_Parse(buf);
    free(buf);
    esp_http_client_cleanup(client);

    if (!resp) {
        ESP_LOGE(TAG, "OTA 响应 JSON 解析失败");
        return -1;
    }

    cJSON *ws = cJSON_GetObjectItem(resp, "websocket");
    if (!ws) {
        ESP_LOGE(TAG, "OTA 响应中无 websocket 字段");
        cJSON_Delete(resp);
        return -1;
    }

    cJSON *url = cJSON_GetObjectItem(ws, "url");
    cJSON *token = cJSON_GetObjectItem(ws, "token");

    if (!cJSON_IsString(url) || !url->valuestring) {
        ESP_LOGE(TAG, "OTA 响应中无 websocket.url");
        cJSON_Delete(resp);
        return -1;
    }

    /* 构造完整的 WebSocket URI（token 拼到查询参数，空格编码为 %20） */
    snprintf(s_ws_url, sizeof(s_ws_url), "%s", url->valuestring);

    if (cJSON_IsString(token) && token->valuestring) {
        snprintf(s_ws_token, sizeof(s_ws_token), "%s", token->valuestring);

        /* 检查 URL 中是否已有参数 */
        char separator = strchr(s_ws_url, '?') ? '&' : '?';

        /* 构造 authorization 值（Bearer%20token） */
        char auth_prefix[384] = {0};
        if (strncmp(s_ws_token, "Bearer ", 7) == 0) {
            snprintf(auth_prefix, sizeof(auth_prefix), "Bearer%%20%s", s_ws_token + 7);
        } else {
            snprintf(auth_prefix, sizeof(auth_prefix), "Bearer%%20%s", s_ws_token);
        }

        /* 拼接到 URL */
        size_t cur_len = strlen(s_ws_url);
        snprintf(s_ws_url + cur_len, sizeof(s_ws_url) - cur_len,
                 "%cauthorization=%s&device-id=%s&client-id=%s",
                 separator, auth_prefix, s_device_mac, s_device_mac);
    }

    cJSON_Delete(resp);
    ESP_LOGI(TAG, "OTA 发现成功: %s", s_ws_url);
    return 0;
}

/* ==================== WebSocket 事件处理 ==================== */

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        if (s_state_cb) s_state_cb(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket 已断开");
        if (s_state_cb) s_state_cb(false);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01 || data->op_code == 0x00) {
            /* 文本帧 */
            if (s_msg_cb && data->data_len > 0) {
                s_msg_cb(data->data_ptr, data->data_len, false);
            }
        } else if (data->op_code == 0x02) {
            /* 二进制帧 */
            if (s_msg_cb && data->data_len > 0) {
                s_msg_cb(data->data_ptr, data->data_len, true);
            }
        } else if (data->op_code == 0x08) {
            ESP_LOGI(TAG, "WebSocket 收到关闭帧");
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket 错误");
        break;

    default:
        break;
    }
}

/* ==================== WebSocket 连接 ==================== */

int ai_chat_ws_connect(void) {
    if (s_ws_client) {
        ESP_LOGW(TAG, "WebSocket 客户端已存在，先断开");
        ai_chat_ws_disconnect();
    }

    if (strlen(s_ws_url) == 0) {
        ESP_LOGE(TAG, "WebSocket URL 为空，请先执行 OTA 发现");
        return -1;
    }

    ESP_LOGI(TAG, "正在连接 WebSocket: %s", s_ws_url);
    ESP_LOGI(TAG, "可用堆内存: 内部=%d, PSRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_websocket_client_config_t ws_cfg = {
        .uri = s_ws_url,
        .buffer_size = 4096,
        .task_stack = 10240,
        .ping_interval_sec = 30,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "WebSocket 客户端初始化失败");
        return -1;
    }

    /* 注册事件处理 */
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    /* 启动连接 */
    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket 启动失败: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        return -1;
    }

    /* 等待连接建立（最多 10 秒） */
    for (int i = 0; i < 100; i++) {
        if (esp_websocket_client_is_connected(s_ws_client)) {
            ESP_LOGI(TAG, "WebSocket 连接成功");
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "WebSocket 连接超时");
    ai_chat_ws_disconnect();
    return -1;
}

void ai_chat_ws_disconnect(void) {
    if (s_ws_client) {
        if (esp_websocket_client_is_connected(s_ws_client)) {
            esp_websocket_client_close(s_ws_client, pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        ESP_LOGI(TAG, "WebSocket 已断开并释放");
    }
}

int ai_chat_ws_send_text(const char *text) {
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        return -1;
    }
    int ret = esp_websocket_client_send_text(s_ws_client, text, strlen(text), pdMS_TO_TICKS(1000));
    return (ret >= 0) ? 0 : -1;
}

int ai_chat_ws_send_binary(const uint8_t *data, int len) {
    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        return -1;
    }
    int ret = esp_websocket_client_send_bin(s_ws_client, (const char *)data, len, pdMS_TO_TICKS(1000));
    return (ret >= 0) ? 0 : -1;
}

bool ai_chat_ws_is_connected(void) {
    return s_ws_client && esp_websocket_client_is_connected(s_ws_client);
}

void ai_chat_ws_set_callbacks(ws_msg_cb_t msg_cb, ws_state_cb_t state_cb) {
    s_msg_cb = msg_cb;
    s_state_cb = state_cb;
}
