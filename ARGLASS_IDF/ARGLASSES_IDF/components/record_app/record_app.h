/**
 * @file record_app.h
 * @brief 音频录音与拍照模块公共接口
 *
 * 本模块提供麦克风录音 (WAV) 和摄像头拍照 (JPEG) 功能。
 * 录音数据来自 sd_ringbuf 环形缓冲区，由 audio_hub_task 写入。
 */

#ifndef RECORD_APP_H
#define RECORD_APP_H

#include "esp_err.h"

/**
 * @brief 开始录音
 *
 * 自动分配文件名 (REC_001.wav, REC_002.wav, ...)，
 * 创建录音文件并启动后台录音线程。
 * 录音数据来自 main.c 中的 sd_ringbuf 环形缓冲区。
 *
 * @return ESP_OK: 成功; ESP_ERR_INVALID_STATE: 已在录音; ESP_FAIL: 文件创建失败
 */
esp_err_t start_record(void);

/**
 * @brief 停止录音
 *
 * 停止后台录音线程，更新 WAV 文件头并关闭文件。
 */
void stop_record(void);

/**
 * @brief 拍照并保存到 SD 卡 /sdcard/拍照/
 *
 * 文件名自动递增: IMG_001.jpg, IMG_002.jpg, ...
 *
 * @return ESP_OK: 成功; ESP_FAIL: 摄像头或文件系统错误
 */
esp_err_t take_photo_and_save(void);

/**
 * @brief 扫描录音文件列表并通过 UART 发送给 UI
 *
 * 扫描 /sdcard/录音/ 下的 .wav 文件，发送 "REC_FILE:文件名" 给 UI。
 */
void scan_and_send_record_list(void);

#endif // RECORD_APP_H
