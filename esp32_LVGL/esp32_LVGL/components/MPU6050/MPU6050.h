#ifndef _MPU6050_H_
#define _MPU6050_H_

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

// 综合初始化接口
esp_err_t mpu6050_init_all(void);

// 读取数据（含姿态解算 + 发送加速度到计步队列）
void read_mpu6050_task(void *pvParameters);

// 独立计步器任务
void step_counter_task(void *pvParameters);

// 计步器全局变量
extern QueueHandle_t accel_queue;
extern uint32_t step_count;

#endif // _MPU6050_H_
