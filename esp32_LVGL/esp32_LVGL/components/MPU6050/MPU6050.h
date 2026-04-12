#ifndef _MPU6050_H_
#define _MPU6050_H_

#include "esp_err.h"

// 综合初始化接口
esp_err_t mpu6050_init_all(void);

// 读取任务
void read_mpu6050_task(void *pvParameters);

#endif // _MPU6050_H_