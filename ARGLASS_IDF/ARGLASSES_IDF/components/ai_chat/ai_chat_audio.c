/**
 * @file ai_chat_audio.c
 * @brief Opus 编解码 + 音频流收发（异步播放）
 *
 * 上行：麦克风 PCM → Opus 编码 → WebSocket 发送
 * 下行：WebSocket 接收 → 入队 → 播放任务解码 → I2S 扬声器
 *
 * 异步播放设计：避免在 WebSocket 回调中长时间占用锁
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "opus.h"
#include "ai_chat_audio.h"
#include "ai_chat_ws.h"
#include "speaker_app.h"
#include "tts_app.h"  // speaker_mutex

static const char *TAG = "AI_AUDIO";

/* ==================== Opus 参数 ==================== */
#define OPUS_SAMPLE_RATE    16000
#define OPUS_CHANNELS       1
#define OPUS_BITRATE        16000
#define OPUS_MAX_FRAME_BYTES 400
#define OPUS_COMPLEXITY      1  // 降到 1，大幅降低 CPU 占用 (语音场景够用)

/* ==================== 异步播放队列 ==================== */
#define PLAY_QUEUE_DEPTH    32      // 队列深度
#define PLAY_TASK_STACK     16384   // 16KB（Opus 解码是栈大户）
#define PLAY_TASK_PRIO      4
#define PLAY_TASK_CORE      0       // 和 TTS 同 Core

typedef struct {
    uint8_t *data;      // Opus 数据指针
    int len;            // 数据长度
} opus_frame_t;

static QueueHandle_t s_play_queue = NULL;
static TaskHandle_t s_play_task_handle = NULL;
static volatile bool s_play_task_running = false;

/* ==================== 编码器/解码器句柄 ==================== */
static OpusEncoder *s_encoder = NULL;
static OpusDecoder *s_decoder = NULL;
static int16_t *s_decode_buffer = NULL;

/* ==================== 播放任务 ==================== */

static void audio_play_task(void *arg) {
    opus_frame_t frame;
    ESP_LOGI(TAG, "音频播放任务已启动");

    while (s_play_task_running) {
        if (xQueueReceive(s_play_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (frame.data && frame.len > 0 && s_decoder && s_decode_buffer) {
                int decoded = opus_decode(s_decoder, frame.data, frame.len,
                                          s_decode_buffer, OPUS_FRAME_SIZE, 0);
                if (decoded > 0) {
                    if (xSemaphoreTake(speaker_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        playSpeaker((const uint8_t *)s_decode_buffer,
                                   decoded * sizeof(int16_t));
                        xSemaphoreGive(speaker_mutex);
                    }
                }
            }
            free(frame.data);  // 释放 Opus 数据
        }
    }

    /* 清空队列 */
    while (xQueueReceive(s_play_queue, &frame, 0) == pdTRUE) {
        if (frame.data) free(frame.data);
    }

    ESP_LOGI(TAG, "音频播放任务已退出");
    vTaskDelete(NULL);
}

/* ==================== 初始化/释放 ==================== */

int ai_chat_audio_init(void) {
    int err;

    /* 创建编码器 */
    s_encoder = opus_encoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS,
                                     OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !s_encoder) {
        ESP_LOGE(TAG, "Opus 编码器创建失败: %d", err);
        return -1;
    }
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    /* 创建解码器 */
    s_decoder = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &err);
    if (err != OPUS_OK || !s_decoder) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d", err);
        opus_encoder_destroy(s_encoder);
        s_encoder = NULL;
        return -1;
    }

    /* 分配解码输出缓冲区（PSRAM） */
    s_decode_buffer = heap_caps_malloc(OPUS_FRAME_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_decode_buffer) {
        ESP_LOGE(TAG, "解码缓冲区分配失败");
        ai_chat_audio_deinit();
        return -1;
    }

    /* 创建播放队列 */
    s_play_queue = xQueueCreate(PLAY_QUEUE_DEPTH, sizeof(opus_frame_t));
    if (!s_play_queue) {
        ESP_LOGE(TAG, "播放队列创建失败");
        ai_chat_audio_deinit();
        return -1;
    }

    /* 启动播放任务（栈在 PSRAM，TCB 在内部 RAM） */
    s_play_task_running = true;
    static StackType_t *play_stack = NULL;
    static StaticTask_t play_tcb;
    if (!play_stack) {
        play_stack = heap_caps_malloc(PLAY_TASK_STACK, MALLOC_CAP_SPIRAM);
    }
    if (play_stack) {
        s_play_task_handle = xTaskCreateStaticPinnedToCore(audio_play_task, "opus_play",
            PLAY_TASK_STACK, NULL, PLAY_TASK_PRIO, play_stack, &play_tcb, PLAY_TASK_CORE);
        ESP_LOGI(TAG, "播放任务栈已分配到 PSRAM (%d bytes)", PLAY_TASK_STACK);
    } else {
        xTaskCreatePinnedToCore(audio_play_task, "opus_play",
            PLAY_TASK_STACK, NULL, PLAY_TASK_PRIO, &s_play_task_handle, PLAY_TASK_CORE);
    }

    ESP_LOGI(TAG, "Opus 编解码器初始化完成 (帧=%d, 比特率=%d)", OPUS_FRAME_SIZE, OPUS_BITRATE);
    return 0;
}

void ai_chat_audio_deinit(void) {
    /* 停止播放任务 */
    s_play_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_encoder) { opus_encoder_destroy(s_encoder); s_encoder = NULL; }
    if (s_decoder) { opus_decoder_destroy(s_decoder); s_decoder = NULL; }
    if (s_decode_buffer) { free(s_decode_buffer); s_decode_buffer = NULL; }
    if (s_play_queue) { vQueueDelete(s_play_queue); s_play_queue = NULL; }
    s_play_task_handle = NULL;

    ESP_LOGI(TAG, "Opus 编解码器已释放");
}

/* ==================== 编码并发送 ==================== */

int ai_chat_audio_encode_and_send(const int16_t *pcm_data, int samples) {
    if (!s_encoder || samples != OPUS_FRAME_SIZE) {
        return -1;
    }

    /* 静音检测 */
    int64_t sum_sq = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = pcm_data[i];
        sum_sq += v * v;
    }
    float rms = (float)(sum_sq / samples);
    if (rms < 100.0f) return 0;

    uint8_t opus_buf[OPUS_MAX_FRAME_BYTES];
    int encoded = opus_encode(s_encoder, pcm_data, OPUS_FRAME_SIZE,
                              opus_buf, OPUS_MAX_FRAME_BYTES);
    if (encoded < 0) return -1;

    int ret = ai_chat_ws_send_binary(opus_buf, encoded);
    return (ret == 0) ? encoded : -1;
}

/* ==================== 接收入队（WebSocket 回调中调用，必须快） ==================== */

void ai_chat_audio_receive_and_play(const uint8_t *opus_data, int opus_len) {
    if (!s_play_queue || opus_len <= 0) return;

    /* 复制 Opus 数据到堆上 */
    uint8_t *copy = malloc(opus_len);
    if (!copy) return;
    memcpy(copy, opus_data, opus_len);

    opus_frame_t frame = { .data = copy, .len = opus_len };

    if (xQueueSend(s_play_queue, &frame, 0) != pdTRUE) {
        /* 队列满，丢弃 */
        free(copy);
    }
}

void ai_chat_audio_flush(void) {
    if (s_play_queue) {
        opus_frame_t frame;
        while (xQueueReceive(s_play_queue, &frame, 0) == pdTRUE) {
            if (frame.data) free(frame.data);
        }
    }
    ESP_LOGI(TAG, "音频刷新完成");
}
