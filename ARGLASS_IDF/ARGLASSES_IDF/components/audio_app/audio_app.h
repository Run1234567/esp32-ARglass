/**
 * @file audio_app.h
 * @brief PDM I2S 麦克风音频采集模块公共接口
 *
 * 本模块负责从 PDM 数字麦克风采集 16kHz/16-bit/单声道 的 PCM 音频数据。
 * 硬件引脚: GPIO 42 (CLK), GPIO 41 (DIN)
 */

#ifndef AUDIO_APP_H
#define AUDIO_APP_H

#include <stdint.h>   // int16_t 类型定义
#include <stddef.h>   // size_t 类型定义

/**
 * @brief 初始化 PDM I2S 麦克风
 *
 * 配置 I2S 通道为 PDM 接收模式，采样率 16kHz，16-bit 量化，单声道。
 * 调用此函数后，麦克风开始工作，可以使用 readAudio() 读取数据。
 *
 * 引脚: GPIO 42 (时钟), GPIO 41 (数据)
 * 必须在 app_main() 中调用一次
 */
void initAudio(void);

/**
 * @brief 从麦克风读取一帧 PCM 音频数据
 *
 * @param buffer  目标缓冲区 (int16_t 数组)，调用者负责分配内存
 * @param samples 要读取的采样点数量 (建议 512)
 * @return 实际读取的字节数 (每个采样点 2 字节)，0 表示超时或出错
 *
 * 注意: 此函数是阻塞式的，最长等待 10ms
 * 典型调用: readAudio(audioBuffer, 512);  // 读取 512 个采样点 = 1024 字节
 */
size_t readAudio(int16_t* buffer, size_t samples);

#endif // AUDIO_APP_H
