/**
 * @file ai_chat_audio.c
 * @brief Opus 编解码 + 音频流收发
 *
 * 上行：麦克风 PCM → Opus 编码 → WebSocket 发送
 * 下行：WebSocket 接收 → Opus 解码 → I2S 扬声器播放
 *
 * Opus 参数（与 digital-human 项目一致）：
 *   - 采样率: 16kHz
 *   - 声道: 单声道
 *   - 帧大小: 960 采样点 (60ms)
 *   - 比特率: 16kbps (编码) / 64kbps (解码)
 *   - 模式: VOIP
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "opus.h"
#include "ai_chat_audio.h"
#include "ai_chat_ws.h"
#include "speaker_app.h"
#include "tts_app.h"  // speaker_mutex 外部引用

static const char *TAG = "AI_AUDIO";

/* ==================== Opus 参数 ==================== */
#define OPUS_SAMPLE_RATE    16000
#define OPUS_CHANNELS       1
/* OPUS_FRAME_SIZE (960) 已在 ai_chat_audio.h 中定义 */
#define OPUS_BITRATE        16000   // 16kbps 编码
#define OPUS_DECODE_BITRATE 64000   // 64kbps 解码
#define OPUS_MAX_FRAME_BYTES 400    // 编码输出最大字节数
#define OPUS_COMPLEXITY      5      // 编码复杂度 (0-10)

/* ==================== 编码器/解码器句柄 ==================== */
static OpusEncoder *s_encoder = NULL;
static OpusDecoder *s_decoder = NULL;

/* 解码输出缓冲区 */
static int16_t *s_decode_buffer = NULL;

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
    opus_encoder_ctl(s_encoder, OPUS_SET_DTX(1));  // 静音时降低带宽
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    /* 创建解码器 */
    s_decoder = opus_decoder_create(OPUS_SAMPLE_RATE, OPUS_CHANNELS, &err);
    if (err != OPUS_OK || !s_decoder) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d", err);
        opus_encoder_destroy(s_encoder);
        s_encoder = NULL;
        return -1;
    }

    /* 分配解码输出缓冲区（使用 PSRAM） */
    s_decode_buffer = heap_caps_malloc(OPUS_FRAME_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_decode_buffer) {
        ESP_LOGE(TAG, "解码缓冲区分配失败");
        ai_chat_audio_deinit();
        return -1;
    }

    ESP_LOGI(TAG, "Opus 编解码器初始化完成 (帧=%d, 比特率=%d)", OPUS_FRAME_SIZE, OPUS_BITRATE);
    return 0;
}

void ai_chat_audio_deinit(void) {
    if (s_encoder) {
        opus_encoder_destroy(s_encoder);
        s_encoder = NULL;
    }
    if (s_decoder) {
        opus_decoder_destroy(s_decoder);
        s_decoder = NULL;
    }
    if (s_decode_buffer) {
        free(s_decode_buffer);
        s_decode_buffer = NULL;
    }
    ESP_LOGI(TAG, "Opus 编解码器已释放");
}

/* ==================== 编码并发送 ==================== */

int ai_chat_audio_encode_and_send(const int16_t *pcm_data, int samples) {
    if (!s_encoder || samples != OPUS_FRAME_SIZE) {
        static int enc_err_log = 0;
        if (enc_err_log < 3) {
            ESP_LOGW(TAG, "编码跳过: encoder=%p samples=%d expected=%d",
                     (void*)s_encoder, samples, OPUS_FRAME_SIZE);
            enc_err_log++;
        }
        return -1;
    }

    /* 静音检测：计算 RMS */
    int64_t sum_sq = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = pcm_data[i];
        sum_sq += v * v;
    }
    float rms = (float)(sum_sq / samples);
    if (rms < 100.0f) {
        /* 基本静音，跳过编码节省 CPU */
        return 0;
    }

    /* Opus 编码 */
    uint8_t opus_buf[OPUS_MAX_FRAME_BYTES];
    int encoded_bytes = opus_encode(s_encoder, pcm_data, OPUS_FRAME_SIZE,
                                    opus_buf, OPUS_MAX_FRAME_BYTES);
    if (encoded_bytes < 0) {
        ESP_LOGW(TAG, "Opus 编码失败: %d", encoded_bytes);
        return -1;
    }

    /* 通过 WebSocket 发送二进制帧 */
    int ret = ai_chat_ws_send_binary(opus_buf, encoded_bytes);
    if (ret != 0) {
        ESP_LOGW(TAG, "Opus 数据发送失败");
        return -1;
    }

    return encoded_bytes;
}

/* ==================== 接收并解码播放 ==================== */

void ai_chat_audio_receive_and_play(const uint8_t *opus_data, int opus_len) {
    if (!s_decoder || !s_decode_buffer) {
        return;
    }

    /* Opus 解码 */
    int decoded_samples = opus_decode(s_decoder, opus_data, opus_len,
                                       s_decode_buffer, OPUS_FRAME_SIZE, 0);
    if (decoded_samples < 0) {
        ESP_LOGW(TAG, "Opus 解码失败: %d", decoded_samples);
        return;
    }

    if (decoded_samples > 0) {
        /* 通过扬声器播放（需要互斥锁保护） */
        if (xSemaphoreTake(speaker_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            playSpeaker((const uint8_t *)s_decode_buffer,
                       decoded_samples * sizeof(int16_t));
            xSemaphoreGive(speaker_mutex);
        }
    }
}

void ai_chat_audio_flush(void) {
    /* 清空操作在当前实现中无需特别处理
     * 因为每次解码后直接播放，没有额外缓冲 */
    ESP_LOGI(TAG, "音频刷新完成");
}
