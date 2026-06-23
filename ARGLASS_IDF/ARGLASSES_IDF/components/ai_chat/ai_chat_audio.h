/**
 * @file ai_chat_audio.h
 * @brief Opus 编解码 + 音频流收发
 */

#ifndef AI_CHAT_AUDIO_H
#define AI_CHAT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* ==================== Opus 常量 ==================== */
#define OPUS_FRAME_SIZE  960  // 每帧采样点数 (60ms @ 16kHz)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Opus 编码器和解码器
 * @return 0=成功, -1=失败
 */
int ai_chat_audio_init(void);

/**
 * @brief 释放 Opus 编码器和解码器
 */
void ai_chat_audio_deinit(void);

/**
 * @brief 编码一帧 PCM 数据为 Opus 并发送
 * @param pcm_data 16-bit PCM 数据
 * @param samples  采样点数（应为 960）
 * @return 编码后的字节数，-1=失败
 */
int ai_chat_audio_encode_and_send(const int16_t *pcm_data, int samples);

/**
 * @brief 接收 Opus 数据并解码为 PCM，通过扬声器播放
 * @param opus_data Opus 编码数据
 * @param opus_len  数据长度
 */
void ai_chat_audio_receive_and_play(const uint8_t *opus_data, int opus_len);

/**
 * @brief 清空音频播放缓冲区
 */
void ai_chat_audio_flush(void);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_AUDIO_H
