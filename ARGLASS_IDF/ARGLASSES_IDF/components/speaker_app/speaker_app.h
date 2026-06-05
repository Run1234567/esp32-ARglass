/**
 * @file speaker_app.h
 * @brief I2S 扬声器输出模块公共接口
 *
 * 本模块提供扬声器初始化和带软件音量控制的音频播放功能。
 * 硬件引脚: GPIO 1 (WS), GPIO 2 (BCK), GPIO 3 (DATA)
 * 音频格式: 16kHz / 16-bit / 单声道
 */

#ifndef SPEAKER_APP_H
#define SPEAKER_APP_H

#include <stdint.h>   // uint8_t, int16_t 类型定义
#include <stddef.h>   // size_t 类型定义

/**
 * @brief 初始化 I2S 扬声器
 *
 * 配置 I2S 标准模式 (Philips 格式)，16kHz 采样率，16-bit，单声道。
 * 调用后扬声器通道启动，可以使用 playSpeaker() 播放音频。
 * 必须在 app_main() 中调用一次。
 */
void initSpeaker(void);

/**
 * @brief 播放 PCM 音频数据 (带软件音量控制)
 *
 * @param data   PCM 音频数据 (16-bit 采样点的字节表示)
 * @param length 数据长度 (字节数)
 *
 * 内部会根据当前音量设置自动进行缩放:
 *   - 音量 0%: 静音，数据直接丢弃
 *   - 音量 100%: 原始输出，无计算开销
 *   - 其他: 逐采样点缩放 + 防爆音裁剪
 *
 * 注意: 如果多个任务同时调用此函数，需要使用 speaker_mutex 互斥锁
 */
void playSpeaker(const uint8_t *data, size_t length);

/**
 * @brief 设置扬声器音量
 * @param vol 音量值 (0-100)，0=静音，100=最大
 */
void set_speaker_volume(uint8_t vol);

/**
 * @brief 获取当前扬声器音量
 * @return 当前音量值 (0-100)
 */
uint8_t get_speaker_volume(void);

#endif // SPEAKER_APP_H
