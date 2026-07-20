#include "mpu6050.h"
#include <stdio.h>
#include <math.h>         // 引入数学库
#include "esp_timer.h"    // 引入 ESP32 高精度定时器
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c.h"
#include "esp_log.h"

// --- 计步器全局变量 ---
QueueHandle_t accel_queue = NULL;
uint32_t step_count = 0;

static const char *TAG = "MPU6050";
// 定义姿态数据及四元数结构体
typedef struct {
    float roll;   // 横滚角
    float pitch;  // 俯仰角
    float yaw;    // 偏航角
    float q0;     // 四元数 q0 (通常初始化为 1.0f)
    float q1;     // 四元数 q1 (通常初始化为 0.0f)
    float q2;     // 四元数 q2 (通常初始化为 0.0f)
    float q3;     // 四元数 q3 (通常初始化为 0.0f)
} attitude_t;
// --- 硬件连接与地址配置 ---
#define I2C_MASTER_SCL_IO           1
#define I2C_MASTER_SDA_IO           2
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define MPU6050_ADDR                0x68
#define MPU6050_PWR_MGMT_1_REG      0x6B
#define MPU6050_ACCEL_XOUT_H_REG    0x3B

// --- 算法需要的宏和全局变量 ---
#define RAD_TO_DEG  57.2957795131f
#define DEG_TO_RAD  0.01745329252f
#define KP 2.0f              // 比例增益控制加速度计收敛速度
#define KI 0.005f            // 积分增益控制陀螺仪零偏消除速度
#define INTEGRAL_LIMIT 2.0f  // 积分限幅

attitude_t attitude = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}; // 初始四元数 q0=1

// 滤波后的数据
static float lpf_acc_x = 0, lpf_acc_y = 0, lpf_acc_z = 0;
static float lpf_gyro_x = 0, lpf_gyro_y = 0, lpf_gyro_z = 0;
// 积分误差
static float integralFBx = 0, integralFBy = 0, integralFBz = 0;
// 上一次的时间戳 (微秒)
static int64_t last_time_us = 0;



static void mpu6050_wake_up(void) {
    uint8_t write_buf[2] = {MPU6050_PWR_MGMT_1_REG, 0x00};
    i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, write_buf, sizeof(write_buf), pdMS_TO_TICKS(1000));
}

esp_err_t mpu6050_init_all(void) {
    mpu6050_wake_up();
    return ESP_OK;
}

// ==========================================
// 核心算法函数：包含滤波与 Mahony 解算
// ==========================================
static void process_imu_data(int16_t ax_raw, int16_t ay_raw, int16_t az_raw, 
                             int16_t gx_raw, int16_t gy_raw, int16_t gz_raw) 
{
    // 1. --- 零偏与死区处理 ---
    // (根据你的板子实际情况，可能需要重新测一下 MPU6050 的静止偏置)
    int16_t gx_adj = gx_raw+1000;
    int16_t gy_adj = gy_raw - 110;
    int16_t gz_adj = gz_raw +=90;
    if(gy_adj <= 50 && gy_adj >= -50) gy_adj = 0;
    if(gx_adj <= 50 && gx_adj >= -50) gx_adj = 0;
    if(gz_adj <= 50 && gz_adj >= -50) gz_adj = 0; 
    else gz_adj -= 1;

    // 2. --- 一阶低通滤波 ---
    float acc_alpha = 0.5f;  
    float gyro_alpha = 0.9f; 

    lpf_acc_x = acc_alpha * ax_raw + (1.0f - acc_alpha) * lpf_acc_x;
    lpf_acc_y = acc_alpha * ay_raw + (1.0f - acc_alpha) * lpf_acc_y;
    lpf_acc_z = acc_alpha * az_raw + (1.0f - acc_alpha) * lpf_acc_z;

    lpf_gyro_x = gyro_alpha * gx_adj + (1.0f - gyro_alpha) * lpf_gyro_x;
    lpf_gyro_y = gyro_alpha * gy_adj + (1.0f - gyro_alpha) * lpf_gyro_y;
    lpf_gyro_z = gyro_alpha * gz_adj + (1.0f - gyro_alpha) * lpf_gyro_z;

    // 3. --- 计算真实时间间隔 dt (ESP32专用方法) ---
    int64_t current_time_us = esp_timer_get_time();
    if (last_time_us == 0) last_time_us = current_time_us; // 第一次初始化
    float dt = (current_time_us - last_time_us) / 1000000.0f; // 转换为秒
    last_time_us = current_time_us;
    
    if (dt > 0.05f || dt <= 0.0f) dt = 0.005f; 

    // 4. --- 转换单位 ---
    // MPU6050 陀螺仪默认量程 ±250°/s，灵敏度 131 LSB/(°/s)
    float gx = (lpf_gyro_x / 131.0f) * DEG_TO_RAD;
    float gy = (lpf_gyro_y / 131.0f) * DEG_TO_RAD;
    float gz = (lpf_gyro_z / 131.0f) * DEG_TO_RAD;

    float ax = lpf_acc_x;
    float ay = lpf_acc_y;
    float az = lpf_acc_z;

    // 5. --- Mahony 姿态解算 ---
    float q0 = attitude.q0, q1 = attitude.q1, q2 = attitude.q2, q3 = attitude.q3;
    float norm_acc = sqrtf(ax*ax + ay*ay + az*az);
    
    if (norm_acc > 0.0f) {
        ax /= norm_acc;
        ay /= norm_acc;
        az /= norm_acc;
        
        float dynamic_KP = KP;
        float dynamic_KI = KI;
        
        // 计算重力方向
        float vx = 2.0f * (q1*q3 - q0*q2);
        float vy = 2.0f * (q0*q1 + q2*q3);
        float vz = q0*q0 - q1*q1 - q2*q2 + q3*q3;
        
        // 计算误差（叉积）
        float ex = (ay*vz - az*vy);
        float ey = (az*vx - ax*vz);
        
        // 🚨 修改点：MPU6050 的 1g 约等于 16384。这里将阈值放宽到 1g ± 20%
        if (norm_acc > 19660 || norm_acc < 13100) { 
            dynamic_KP = 0.0f;  
            dynamic_KI = 0.0f;  
        } else {
            integralFBx += dynamic_KI * ex * dt;
            integralFBy += dynamic_KI * ey * dt;
        }
        
        // 积分限幅
        if (integralFBx > INTEGRAL_LIMIT) integralFBx = INTEGRAL_LIMIT;
        if (integralFBx < -INTEGRAL_LIMIT) integralFBx = -INTEGRAL_LIMIT;
        if (integralFBy > INTEGRAL_LIMIT) integralFBy = INTEGRAL_LIMIT;
        if (integralFBy < -INTEGRAL_LIMIT) integralFBy = -INTEGRAL_LIMIT;
        
        // PI 控制陀螺仪
        gx += dynamic_KP * ex + integralFBx;
        gy += dynamic_KP * ey + integralFBy;
    }
    
    // 四元数微分方程更新
    attitude.q0 += (-q1*gx - q2*gy - q3*gz) * 0.5f * dt;
    attitude.q1 += ( q0*gx - q3*gy + q2*gz) * 0.5f * dt;
    attitude.q2 += ( q3*gx + q0*gy - q1*gz) * 0.5f * dt;
    attitude.q3 += (-q2*gx + q1*gy + q0*gz) * 0.5f * dt;
    
    // 四元数归一化
    float norm = sqrtf(attitude.q0*attitude.q0 + attitude.q1*attitude.q1 + 
                       attitude.q2*attitude.q2 + attitude.q3*attitude.q3);
    if(norm > 0.0f) {
        norm = 1.0f / norm;
        attitude.q0 *= norm;
        attitude.q1 *= norm;
        attitude.q2 *= norm;
        attitude.q3 *= norm;
    }
    
    // 计算欧拉角
    attitude.roll = atan2f(2*(attitude.q0*attitude.q1 + attitude.q2*attitude.q3), 
                           1 - 2*(attitude.q1*attitude.q1 + attitude.q2*attitude.q2)) * RAD_TO_DEG;
    attitude.pitch = asinf(2*(attitude.q0*attitude.q2 - attitude.q3*attitude.q1)) * RAD_TO_DEG;
    attitude.yaw = atan2f(2*(attitude.q0*attitude.q3 + attitude.q1*attitude.q2),
                          1 - 2*(attitude.q2*attitude.q2 + attitude.q3*attitude.q3)) * RAD_TO_DEG;
}

// ==========================================
// 任务函数：独立计步器
// ==========================================
void step_counter_task(void *pvParameters) {
    float accel_val;
    float accel_buffer[5] = {0}; // 5点滑动平均缓存
    uint8_t buf_idx = 0;

    // MPU6050 ±2g 量程下，1g 约等于 16384
    float max_val = 0;
    float min_val = 32768.0f;
    float threshold = 32000.0f; // 初始阈值（约2g，需要明显晃动才计步）

    float last_smoothed_accel = 32000.0f;
    uint32_t sample_count = 0;
    int64_t last_step_time = 0;
    uint32_t debug_count = 0;

    // 动态阈值更新窗口大小
    const uint32_t WINDOW_SIZE = 50;
    // 防抖：步伐之间的最小间隔，600ms 对应极限 1.7步/秒（正常走路约1.5-2步/秒）
    const int64_t MIN_STEP_DELAY_US = 600000;

    ESP_LOGI("STEP", "计步器任务已启动，等待数据...");

    while (1) {
        // 阻塞等待队列中的加速度数据（由 MPU6050 读取任务发送）
        if (xQueueReceive(accel_queue, &accel_val, portMAX_DELAY) == pdTRUE) {

            // 调试：打印原始加速度值（每100次打印一次）
            if (++debug_count >= 100) {
                ESP_LOGI("STEP", "原始加速度: %.1f | 阈值: %.1f", accel_val, threshold);
                debug_count = 0;
            }

            // 1. 滑动平均滤波
            accel_buffer[buf_idx] = accel_val;
            buf_idx = (buf_idx + 1) % 5;
            float smoothed_accel = 0;
            for(int i = 0; i < 5; i++) {
                smoothed_accel += accel_buffer[i];
            }
            smoothed_accel /= 5.0f;

            // 2. 更新最大值与最小值
            if (smoothed_accel > max_val) max_val = smoothed_accel;
            if (smoothed_accel < min_val) min_val = smoothed_accel;

            sample_count++;

            // 3. 动态更新阈值
            if (sample_count >= WINDOW_SIZE) {
                // 如果这段时间内的震动幅度足够大（大于 1500 LSB，过滤微小抖动）
                if ((max_val - min_val) > 1500.0f) {
                    threshold = (max_val + min_val) / 2.0f;
                } else {
                    threshold = 16384.0f; // 震动太小，恢复基准重力阈值
                }
                // 重置统计变量
                max_val = 0;
                min_val = 32768.0f;
                sample_count = 0;
            }

            // 4. 波峰检测与时间防抖
            int64_t current_time = esp_timer_get_time();
            // 上升沿穿过阈值
            if (smoothed_accel > threshold && last_smoothed_accel <= threshold) {
                if ((current_time - last_step_time) > MIN_STEP_DELAY_US) {
                    step_count++; // 有效计步
                    last_step_time = current_time;
                    ESP_LOGI("STEP", "检测到步伐！当前总步数: %lu", step_count);
                }
            }

            last_smoothed_accel = smoothed_accel;
        }
    }
}

// ==========================================
// 任务函数：不断读取，不断解算
// ==========================================
void read_mpu6050_task(void *pvParameters) {
    uint8_t raw_data[14]; 
    while (1) {
        esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, 
                                                     (uint8_t[]){MPU6050_ACCEL_XOUT_H_REG}, 1, 
                                                     raw_data, sizeof(raw_data), 
                                                     pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            int16_t accel_x = (raw_data[0] << 8) | raw_data[1];
            int16_t accel_y = (raw_data[2] << 8) | raw_data[3];
            int16_t accel_z = (raw_data[4] << 8) | raw_data[5];
            int16_t gyro_x = (raw_data[8] << 8) | raw_data[9];
            int16_t gyro_y = (raw_data[10] << 8) | raw_data[11];
            int16_t gyro_z = (raw_data[12] << 8) | raw_data[13];

            // 🚀 将读到的数据直接扔进解算器
            process_imu_data(accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z);

            // ----------------------------------------------------
            // 🚀 计算三轴加速度总模长，并发送给计步任务
            // ----------------------------------------------------
            float raw_norm_acc = sqrtf((float)accel_x*accel_x + (float)accel_y*accel_y + (float)accel_z*accel_z);
            if (accel_queue != NULL) {
                // 等待时间设为0，防止队列满时阻塞IMU数据读取
                xQueueSend(accel_queue, &raw_norm_acc, 0);
            }
            // ----------------------------------------------------

            // 串口输出陀螺仪数据（每500次打印一次，约5秒 @200Hz）
            static int print_count = 0;
            if (++print_count >= 500) {
                ESP_LOGI(TAG, "MPU6050 | Accel: X=%d Y=%d Z=%d | Gyro: X=%d Y=%d Z=%d",
                         accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z);
                ESP_LOGI(TAG, "MPU6050 | Roll: %.1f | Pitch: %.1f | Yaw: %.1f",
                         attitude.roll, attitude.pitch, attitude.yaw);
                print_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 解算频率提升至 200Hz (5ms)，对 Mahony 算法非常重要！
    }
}