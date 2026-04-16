#ifndef RECORD_APP_H
#define RECORD_APP_H

#include "esp_err.h"

// ? 一键开始录音 (自动命名 REC_xxx.wav)
esp_err_t start_record(void);

// ? 停止录音
void stop_record(void);

// ? 拍照功能接口 (一键抓拍并自动命名 IMG_xxx.jpg)
esp_err_t take_photo_and_save(void);

#endif // RECORD_APP_H