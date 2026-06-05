// ============================================================
// light_sensor.h
// J.A.R.V.I.S. AR 智能眼镜 —— 光照传感器驱动头文件
// ============================================================
// 硬件：TEMT6000 环境光传感器
// 引脚：GPIO 4（ADC1_CH3）
// ============================================================

#ifndef _LIGHT_SENSOR_H
#define _LIGHT_SENSOR_H

#include "esp_err.h"

// 初始化光照传感器（配置 ADC + 启动后台采集任务）
// 返回：ESP_OK = 成功
esp_err_t light_sensor_init(void);

// 获取最新的光照强度（Lux，由后台任务持续更新，线程安全）
// 返回：近似光照强度（Lux），0 = 完全黑暗
float light_sensor_get_lux(void);

// 获取最新的 ADC 原始值（0~4095，由后台任务持续更新，线程安全）
// 返回：12 位 ADC 原始数值
int light_sensor_get_raw(void);

// 释放光照传感器资源（停止后台任务 + 释放 ADC）
esp_err_t light_sensor_deinit(void);

#endif // _LIGHT_SENSOR_H
