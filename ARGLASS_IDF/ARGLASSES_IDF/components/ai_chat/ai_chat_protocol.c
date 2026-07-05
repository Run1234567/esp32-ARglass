/**
 * @file ai_chat_protocol.c
 * @brief 消息协议处理（hello/listen/abort/tts/stt/llm/mcp）
 *
 * 参照 digital-human 项目 websocket.js 的消息格式实现。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "ai_chat_protocol.h"
#include "ai_chat_ws.h"
#include "ai_chat_audio.h"
#include "my_uart.h"   // UI MCU 字幕发送

static const char *TAG = "AI_PROTO";

/* ==================== 会话状态 ==================== */
static char s_session_id[64] = {0};       // 服务器分配的会话 ID
static bool s_server_speaking = false;     // 服务器是否正在说话

/* ==================== Hello 握手 ==================== */

int proto_send_hello(void) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "hello");
    cJSON_AddNumberToObject(msg, "version", 3);
    cJSON_AddStringToObject(msg, "transport", "websocket");

    /* 告诉服务器我们发的是 Opus 音频 */
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "opus");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channels", 1);
    cJSON_AddNumberToObject(audio, "frame_duration", 60);
    cJSON_AddItemToObject(msg, "audio_params", audio);

    cJSON_AddStringToObject(msg, "device_id", mac_str);
    cJSON_AddStringToObject(msg, "device_name", "ARGLASS-JARVIS");
    cJSON_AddStringToObject(msg, "device_mac", mac_str);
    cJSON_AddStringToObject(msg, "token", "");

    cJSON *features = cJSON_CreateObject();
    cJSON_AddBoolToObject(features, "mcp", false);
    cJSON_AddBoolToObject(features, "emoji", false);
    cJSON_AddItemToObject(msg, "features", features);

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);

    ESP_LOGI(TAG, "发送 hello: %s", json);
    int ret = ai_chat_ws_send_text(json);
    free(json);
    return ret;
}

/* ==================== Listen 消息 ==================== */

int proto_send_listen_start(void) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "listen");
    cJSON_AddStringToObject(msg, "state", "start");
    cJSON_AddStringToObject(msg, "mode", "auto");
    if (strlen(s_session_id) > 0) {
        cJSON_AddStringToObject(msg, "session_id", s_session_id);
    }

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);

    ESP_LOGI(TAG, "发送 listen start");
    int ret = ai_chat_ws_send_text(json);
    free(json);
    return ret;
}

int proto_send_listen_detect(const char *text) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "listen");
    cJSON_AddStringToObject(msg, "state", "detect");
    cJSON_AddStringToObject(msg, "text", text);
    if (strlen(s_session_id) > 0) {
        cJSON_AddStringToObject(msg, "session_id", s_session_id);
    }

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);

    ESP_LOGI(TAG, "发送 listen detect: %s", text);
    int ret = ai_chat_ws_send_text(json);
    free(json);
    return ret;
}

int proto_send_abort(void) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "abort");
    cJSON_AddStringToObject(msg, "reason", "wake_word_detected");
    if (strlen(s_session_id) > 0) {
        cJSON_AddStringToObject(msg, "session_id", s_session_id);
    }

    char *json = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);

    ESP_LOGI(TAG, "发送 abort");
    int ret = ai_chat_ws_send_text(json);
    free(json);
    return ret;
}

/* ==================== MCP 简化响应 ==================== */

static void handle_mcp_message(cJSON *msg) {
    cJSON *payload = cJSON_GetObjectItem(msg, "payload");
    if (!payload) return;

    cJSON *method = cJSON_GetObjectItem(payload, "method");
    if (!method || !cJSON_IsString(method)) return;

    cJSON *id = cJSON_GetObjectItem(payload, "id");

    if (strcmp(method->valuestring, "initialize") == 0) {
        /* 回复 MCP 初始化 */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "mcp");
        cJSON_AddStringToObject(reply, "session_id", s_session_id);

        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(rp, "id", cJSON_Duplicate(id, true));

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON *caps = cJSON_CreateObject();
        cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddItemToObject(result, "capabilities", caps);
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "name", "xiaozhi-esp32s3");
        cJSON_AddStringToObject(info, "version", "1.0.0");
        cJSON_AddItemToObject(result, "serverInfo", info);
        cJSON_AddItemToObject(rp, "result", result);
        cJSON_AddItemToObject(reply, "payload", rp);

        char *json = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        ESP_LOGI(TAG, "回复 MCP initialize");
        ai_chat_ws_send_text(json);
        free(json);

    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        /* 回复空工具列表 */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "mcp");
        cJSON_AddStringToObject(reply, "session_id", s_session_id);

        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(rp, "id", cJSON_Duplicate(id, true));

        cJSON *result = cJSON_CreateObject();
        cJSON *tools = cJSON_CreateArray();
        cJSON_AddItemToObject(result, "tools", tools);
        cJSON_AddItemToObject(rp, "result", result);
        cJSON_AddItemToObject(reply, "payload", rp);

        char *json = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        ESP_LOGI(TAG, "回复 MCP tools/list（空列表）");
        ai_chat_ws_send_text(json);
        free(json);

    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        /* 回复工具调用失败（不支持） */
        cJSON *params = cJSON_GetObjectItem(payload, "params");
        cJSON *tool_name = params ? cJSON_GetObjectItem(params, "name") : NULL;
        ESP_LOGW(TAG, "收到 MCP 工具调用: %s（不支持）",
                 tool_name && tool_name->valuestring ? tool_name->valuestring : "unknown");

        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "mcp");
        cJSON_AddStringToObject(reply, "session_id", s_session_id);

        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(rp, "id", cJSON_Duplicate(id, true));

        cJSON *error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32601);
        cJSON_AddStringToObject(error, "message", "Method not found");
        cJSON_AddItemToObject(rp, "error", error);
        cJSON_AddItemToObject(reply, "payload", rp);

        char *json = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        ai_chat_ws_send_text(json);
        free(json);
    }
}

/* ==================== 服务器消息分发 ==================== */

void proto_handle_server_text(const char *data, int len) {
    /* 确保 null 结尾 */
    char *buf = malloc(len + 1);
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *msg = cJSON_Parse(buf);

    if (!msg) {
        ESP_LOGW(TAG, "收到非 JSON 消息 (可能是报错): %s", buf);
        free(buf);
        return;
    }
    free(buf);

    cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(msg);
        return;
    }

    const char *type_str = type->valuestring;

    if (strcmp(type_str, "hello") == 0) {
        /* Hello 握手响应 */
        cJSON *sid = cJSON_GetObjectItem(msg, "session_id");
        if (sid && cJSON_IsString(sid)) {
            strncpy(s_session_id, sid->valuestring, sizeof(s_session_id) - 1);
            ESP_LOGI(TAG, "握手成功，会话 ID: %s", s_session_id);
        }
        /* 握手成功后发送 listen detect + listen start */
        proto_send_listen_detect("嘿，你好呀");
        proto_send_listen_start();

    } else if (strcmp(type_str, "tts") == 0) {
        /* TTS 状态消息 */
        cJSON *state = cJSON_GetObjectItem(msg, "state");
        cJSON *text = cJSON_GetObjectItem(msg, "text");

        if (state && cJSON_IsString(state)) {
            if (strcmp(state->valuestring, "start") == 0) {
                ESP_LOGI(TAG, "🔊 TTS 开始");
                s_server_speaking = true;
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "🔊 TTS 结束");
                s_server_speaking = false;
                ai_chat_audio_flush();
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                if (text && cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "🗣️ AI: %s", text->valuestring);
                    /* 发送字幕给 UI MCU */
                    ui_update_subtitle(text->valuestring);
                }
            }
        }

    } else if (strcmp(type_str, "stt") == 0) {
        /* 语音识别结果 */
        cJSON *text = cJSON_GetObjectItem(msg, "text");
        if (text && cJSON_IsString(text)) {
            ESP_LOGI(TAG, "🎤 识别: %s", text->valuestring);
            /* 检查是否需要绑定设备 */
            if (strstr(text->valuestring, "绑定") || strstr(text->valuestring, "bind")) {
                ESP_LOGW(TAG, "===========================================");
                ESP_LOGW(TAG, "⚠️  设备未绑定！请在服务器上绑定此设备");
                ESP_LOGW(TAG, "⚠️  收到的提示: %s", text->valuestring);
                ESP_LOGW(TAG, "===========================================");
            }
        }

    } else if (strcmp(type_str, "llm") == 0) {
        /* 大模型回复 */
        cJSON *text = cJSON_GetObjectItem(msg, "text");
        if (text && cJSON_IsString(text)) {
            ESP_LOGI(TAG, "🤖 AI: %s", text->valuestring);
        }

    } else if (strcmp(type_str, "mcp") == 0) {
        /* MCP 消息 */
        handle_mcp_message(msg);

    } else if (strcmp(type_str, "audio") == 0) {
        ESP_LOGI(TAG, "收到音频控制消息");

    } else {
        ESP_LOGW(TAG, "未知消息类型: %s", type_str);
    }

    cJSON_Delete(msg);
}

void proto_handle_server_binary(const char *data, int len) {
    /* 服务器下发的 Opus 音频帧 */
    ai_chat_audio_receive_and_play((const uint8_t *)data, len);
}

void proto_reset(void) {
    memset(s_session_id, 0, sizeof(s_session_id));
    s_server_speaking = false;
}
