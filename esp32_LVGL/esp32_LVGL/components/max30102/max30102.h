// ============================================================
// max30102.h
// J.A.R.V.I.S. AR 智能眼镜 —— MAX30102 心率血氧传感器
// ============================================================
// 引脚：GPIO 1 (SCL), GPIO 2 (SDA) —— 与其他 I2C 设备共享总线
// ============================================================

#ifndef _MAX30102_H
#define _MAX30102_H

#include "esp_err.h"

// 初始化 MAX30102（I2C 总线 + 传感器配置）
esp_err_t max30102_init(void);

// 启动后台心率血氧采集任务
esp_err_t max30102_start_task(void);

// 获取最新心率 (BPM)
float max30102_get_bpm(void);

// 获取最新血氧 (%)
float max30102_get_spo2(void);

// 释放资源
esp_err_t max30102_deinit(void);

#endif // _MAX30102_H
