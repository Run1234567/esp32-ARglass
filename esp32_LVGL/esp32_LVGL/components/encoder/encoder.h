#ifndef _ENCODER_H
#define _ENCODER_H

#include "esp_err.h"

// 旋转编码器初始化（GPIO 47 = A相, GPIO 48 = B相）
esp_err_t encoder_init(void);

// 获取编码器计数值（正转增加，反转减少）
int32_t encoder_get_count(void);

// 清零计数
void encoder_reset(void);

#endif
