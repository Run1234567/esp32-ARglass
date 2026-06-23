/**
 * @file ai_chat.c
 * @brief AI 语音对话模块 - 主逻辑（状态机 + FreeRTOS 任务）
 *
 * 完整流程：
 *   1. 唤醒词检测到后，voice_app 调用 ai_chat_start()
 *   2. ai_chat_task 执行 OTA 发现 → WebSocket 连接 → Hello 握手
 *   3. 握手成功后，从 ai_ringbuf 读取麦克风音频 → Opus 编码 → WS 发送
 *   4. 接收服务器下发的 Opus 音频 → 解码 → 播放
 *   5. 接收 JSON 消息（stt/llm/tts/mcp）并处理
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "ai_chat.h"
#include "ai_chat_ws.h"
#include "ai_chat_protocol.h"
#include "ai_chat_audio.h"

static const char *TAG = "AI_CHAT";

/* ==================== 任务配置 ==================== */
#define AI_CHAT_TASK_STACK   32768   // 32KB 栈（Opus 编码需要大栈空间）
#define AI_CHAT_TASK_PRIO    5
#define AI_CHAT_TASK_CORE    0       // Core 0（与语音处理同核）

/* ==================== 外部引用 ==================== */
extern RingbufHandle_t ai_ringbuf;   // AI 音频环形缓冲区（在 main.c 中创建）

/* ==================== 内部状态 ==================== */
static volatile ai_chat_state_t s_state = AI_CHAT_IDLE;
static TaskHandle_t s_task_handle = NULL;
static StaticTask_t s_ai_tcb;
static SemaphoreHandle_t s_start_sem = NULL;  // 启动信号量

/* ==================== 语音活动检测 (VAD) ==================== */
static int s_silence_frames = 0;         // 连续静音帧计数
#define SILENCE_THRESHOLD   100.0f       // 静音阈值（RMS）
#define SILENCE_FRAME_LIMIT 50           // 连续 50 帧静音后停止发送 (约 3 秒)

/* ==================== WebSocket 回调 ==================== */

static void on_ws_message(const char *data, int len, bool is_binary) {
    if (is_binary) {
        proto_handle_server_binary(data, len);
    } else {
        proto_handle_server_text(data, len);
    }
}

static void on_ws_state_change(bool connected) {
    if (!connected) {
        ESP_LOGW(TAG, "WebSocket 连接断开");
        s_state = AI_CHAT_IDLE;
    }
}

/* ==================== AI 对话主任务 ==================== */

static void ai_chat_task(void *arg) {
    ESP_LOGI(TAG, "AI 对话任务已启动，等待唤醒信号...");

    while (1) {
        /* 等待启动信号（由 ai_chat_start() 给出） */
        if (xSemaphoreTake(s_start_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "=========================");
        ESP_LOGI(TAG, "AI 对话流程开始");
        ESP_LOGI(TAG, "=========================");

        s_state = AI_CHAT_CONNECTING;

        /* ---- 步骤 0: 等待 WiFi 就绪 ---- */
        ESP_LOGI(TAG, "步骤 0: 等待 WiFi 就绪 (5 秒)...");
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* ---- 步骤 1: OTA 发现（带重试） ---- */
        ESP_LOGI(TAG, "步骤 1: OTA 发现...");
        int ota_retry = 0;
        while (ota_retry < 3) {
            if (ai_chat_ota_discover() == 0) {
                break;
            }
            ota_retry++;
            ESP_LOGW(TAG, "OTA 发现失败，重试 %d/3...", ota_retry);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (ota_retry >= 3) {
            ESP_LOGE(TAG, "OTA 发现最终失败");
            s_state = AI_CHAT_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            s_state = AI_CHAT_IDLE;
            continue;
        }

        /* ---- 步骤 2: WebSocket 连接 ---- */
        ESP_LOGI(TAG, "步骤 2: WebSocket 连接...");
        if (ai_chat_ws_connect() != 0) {
            ESP_LOGE(TAG, "WebSocket 连接失败");
            s_state = AI_CHAT_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            s_state = AI_CHAT_IDLE;
            continue;
        }

        s_state = AI_CHAT_CONNECTED;
        ESP_LOGI(TAG, "WebSocket 连接成功，开始对话");

        /* ---- 步骤 3: 初始化 Opus 编解码器 ---- */
        if (ai_chat_audio_init() != 0) {
            ESP_LOGE(TAG, "Opus 初始化失败");
            ai_chat_ws_disconnect();
            s_state = AI_CHAT_ERROR;
            vTaskDelay(pdMS_TO_TICKS(2000));
            s_state = AI_CHAT_IDLE;
            continue;
        }

        /* ---- 步骤 4: 发送 Hello 握手 ---- */
        ESP_LOGI(TAG, "步骤 3: Hello 握手...");
        proto_send_hello();
        /* 注意：握手响应和 listen 消息在 on_ws_message 回调中处理 */

        s_state = AI_CHAT_STREAMING;
        __asm__ __volatile__("memw" ::: "memory");  // 跨核内存屏障
        s_silence_frames = 0;

        /* ---- 步骤 5: 音频流主循环 ---- */
        ESP_LOGI(TAG, "进入音频流模式, ai_ringbuf=%p, state=%d", (void*)ai_ringbuf, s_state);
        size_t item_size;
        int send_count = 0;
        int skip_count = 0;
        int encode_err_count = 0;

        /* 累积缓冲区：Hub 每次写 512 采样点，Opus 需要 960 采样点 */
        int16_t *accum_buf = heap_caps_malloc(OPUS_FRAME_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        int accum_samples = 0;

        while (s_state == AI_CHAT_STREAMING && ai_chat_ws_is_connected()) {
            void *audio_data = xRingbufferReceive(ai_ringbuf, &item_size, pdMS_TO_TICKS(100));

            if (audio_data != NULL) {
                int samples = item_size / sizeof(int16_t);
                int16_t *src = (int16_t *)audio_data;

                /* 将新数据追加到累积缓冲区 */
                int remaining = samples;
                while (remaining > 0) {
                    int space = OPUS_FRAME_SIZE - accum_samples;
                    int copy = (remaining < space) ? remaining : space;
                    memcpy(&accum_buf[accum_samples], src, copy * sizeof(int16_t));
                    accum_samples += copy;
                    src += copy;
                    remaining -= copy;

                    /* 累积满一帧，编码发送 */
                    if (accum_samples >= OPUS_FRAME_SIZE) {
                        int ret = ai_chat_audio_encode_and_send(accum_buf, OPUS_FRAME_SIZE);
                        if (ret > 0) {
                            s_silence_frames = 0;
                            send_count++;
                        } else if (ret == 0) {
                            s_silence_frames++;
                            skip_count++;
                        } else {
                            encode_err_count++;
                        }
                        accum_samples = 0;
                    }
                }
                vRingbufferReturnItem(ai_ringbuf, audio_data);
            }

            /* 每 5 秒打印一次音频流状态 */
            static int log_cnt = 0;
            log_cnt++;
            if (log_cnt % 50 == 0) {
                ESP_LOGI(TAG, "音频流: 发送=%d 跳过=%d 错误=%d ws=%d",
                         send_count, skip_count, encode_err_count, ai_chat_ws_is_connected());
            }
        }

        if (accum_buf) free(accum_buf);
        ESP_LOGI(TAG, "音频流循环结束: state=%d, ws=%d, 发送=%d帧",
                 s_state, ai_chat_ws_is_connected(), send_count);

        /* ---- 清理 ---- */
        ESP_LOGI(TAG, "音频流结束，清理资源...");
        ai_chat_audio_flush();
        ai_chat_audio_deinit();

        if (ai_chat_ws_is_connected()) {
            /* 发送空二进制帧作为结束标志 */
            uint8_t empty = 0;
            ai_chat_ws_send_binary(&empty, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            ai_chat_ws_disconnect();
        }

        proto_reset();
        s_state = AI_CHAT_IDLE;
        ESP_LOGI(TAG, "AI 对话流程结束");
    }
}

/* ==================== 公共接口 ==================== */

void ai_chat_init(void) {
    /* 创建启动信号量 */
    s_start_sem = xSemaphoreCreateBinary();
    if (!s_start_sem) {
        ESP_LOGE(TAG, "启动信号量创建失败");
        return;
    }

    /* 设置 WebSocket 回调 */
    ai_chat_ws_set_callbacks(on_ws_message, on_ws_state_change);

    /* 创建主任务（栈分配到 PSRAM，释放内部 RAM） */
    void *ai_stack = heap_caps_malloc(AI_CHAT_TASK_STACK, MALLOC_CAP_SPIRAM);
    if (ai_stack) {
        s_task_handle = xTaskCreateStaticPinnedToCore(
            ai_chat_task, "ai_chat", AI_CHAT_TASK_STACK,
            NULL, AI_CHAT_TASK_PRIO, (StackType_t *)ai_stack, &s_ai_tcb, AI_CHAT_TASK_CORE);
        ESP_LOGI(TAG, "AI 对话任务栈已分配到 PSRAM (%d bytes)", AI_CHAT_TASK_STACK);
    } else {
        ESP_LOGW(TAG, "PSRAM 分配失败，尝试内部 RAM");
        BaseType_t ret = xTaskCreatePinnedToCore(ai_chat_task, "ai_chat", AI_CHAT_TASK_STACK,
                                NULL, AI_CHAT_TASK_PRIO, &s_task_handle, AI_CHAT_TASK_CORE);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "任务创建失败! ret=%d", ret);
        }
    }

    /* 打印设备 MAC 地址，用于服务器绑定 */
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "设备 MAC 地址: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "请在服务器上绑定此 MAC 地址后再使用 AI 对话");
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "AI 对话模块初始化完成");
}

void ai_chat_start(void) {
    if (s_state != AI_CHAT_IDLE) {
        ESP_LOGW(TAG, "AI 对话已在运行中，忽略启动请求");
        return;
    }
    if (s_start_sem) {
        xSemaphoreGive(s_start_sem);
        ESP_LOGI(TAG, "AI 对话启动信号已发送");
    }
}

void ai_chat_stop(void) {
    if (s_state == AI_CHAT_IDLE) {
        return;
    }
    /* 断开 WebSocket，主循环会检测到并退出 */
    ai_chat_ws_disconnect();
    ESP_LOGI(TAG, "AI 对话停止信号已发送");
}

bool ai_chat_is_active(void) {
    return s_state != AI_CHAT_IDLE;
}

ai_chat_state_t ai_chat_get_state(void) {
    return s_state;
}
