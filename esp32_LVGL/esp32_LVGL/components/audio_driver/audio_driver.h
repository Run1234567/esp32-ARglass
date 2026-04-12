#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S 音频驱动
 * 默认配置：采样率 44.1kHz，16位双声道，适配 MAX98357
 * * @return 
 * - ESP_OK: 初始化成功
 * - 其他: 初始化失败的错误码
 */
esp_err_t audio_driver_init(void);

/**
 * @brief 播放 PCM 音频数据
 * * @param audio_data 指向音频数据的指针
 * @param len        数据字节长度
 * * @return 
 * - ESP_OK: 写入成功
 * - ESP_ERR_INVALID_STATE: 驱动未初始化
 * - 其他: 写入底层缓冲区的错误码
 */
esp_err_t audio_driver_play(const void *audio_data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_DRIVER_H