#include "mpu6050.h"
#include <stdio.h>
#include <math.h>         // 引入数学库
#include "esp_timer.h"    // 引入 ESP32 高精度定时器
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"

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
#define I2C_MASTER_FREQ_HZ          400000
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

            // static int print_count = 0;
            // if (++print_count >= 20) { 
            //     ESP_LOGI(TAG, "Roll: %.1f | Pitch: %.1f | Yaw: %.1f", attitude.roll, attitude.pitch, attitude.yaw);
            //     print_count = 0;
            // }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 解算频率提升至 200Hz (5ms)，对 Mahony 算法非常重要！
    }
}