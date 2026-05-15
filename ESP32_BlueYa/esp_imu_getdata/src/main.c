#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"

#define SDA_PIN 12
#define SCL_PIN 13
#define I2C_PORT I2C_NUM_0
#define MPU_ADDR 0x68

// 简单的 I2C 写寄存器函数
void mpu_write(uint8_t reg, uint8_t data) {
    uint8_t buf[] = {reg, data};
    i2c_master_write_to_device(I2C_PORT, MPU_ADDR, buf, 2, pdMS_TO_TICKS(50));
}

void app_main(void) {
    // 1. I2C 初始化
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = SDA_PIN;
    conf.scl_io_num = SCL_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    i2c_param_config(I2C_PORT, &conf);
    i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);

    // 2. MPU6050 配置：唤醒并设置量程
    mpu_write(0x6B, 0x00); // 唤醒
    mpu_write(0x1B, 0x08); // 陀螺仪 ±500°/s
    mpu_write(0x1C, 0x08); // 加速度计 ±4g

    uint8_t raw[14];
    while (1) {
        // 读取 14 字节（加速度3轴 + 温度 + 陀螺仪3轴）
        if (i2c_master_write_read_device(I2C_PORT, MPU_ADDR, (uint8_t[]){0x3B}, 1, raw, 14, pdMS_TO_TICKS(50)) == ESP_OK) {
            // 解析数据并加入你的硬件零偏校准
            int16_t ax = (raw[0] << 8) | raw[1];
            int16_t ay = (raw[2] << 8) | raw[3];
            int16_t az = (raw[4] << 8) | raw[5];
            int16_t gx = (int16_t)((raw[8] << 8) | raw[9]) + 478;
            int16_t gy = (int16_t)((raw[10] << 8) | raw[11]) + 100;
            int16_t gz = (int16_t)((raw[12] << 8) | raw[13]) + 20;

            // 严格按照 Python 脚本要求的格式打印
            printf("IMU\n%d, %d, %d, %d, %d, %d\n", ax, ay, az, gx, gy, gz);
        }
        
        // 10ms 延时，确保 100Hz 采样率
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}