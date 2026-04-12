#ifndef MAIN_MAX30105_H_
#define MAIN_MAX30105_H_

#include "driver/i2c.h"
#include "esp_err.h"

// MAX30105 I2C 地址
#define MAX30105_I2C_ADDR 0x57

// 常用寄存器地址
#define MAX30105_REG_FIFO_WR_PTR    0x04
#define MAX30105_REG_OVF_COUNTER    0x05
#define MAX30105_REG_FIFO_RD_PTR    0x06
#define MAX30105_REG_FIFO_DATA      0x07
#define MAX30105_REG_MODE_CONFIG    0x09
#define MAX30105_REG_SPO2_CONFIG    0x0A
#define MAX30105_REG_LED1_PA        0x0C // Red LED
#define MAX30105_REG_LED2_PA        0x0D // IR LED

/**
 * @brief 初始化 MAX30105
 * @param i2c_num 你已经初始化的 I2C 端口号 (例如 I2C_NUM_0)
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t max30105_init(i2c_port_t i2c_num);

/**
 * @brief 从 FIFO 读取最新的红光(Red)和红外光(IR)原始数据
 * @param i2c_num I2C 端口号
 * @param red 存放红光数据的指针
 * @param ir  存放红外光数据的指针
 * @return esp_err_t 成功返回 ESP_OK
 */
esp_err_t max30105_read_fifo(i2c_port_t i2c_num, uint32_t *red, uint32_t *ir);

#endif /* MAIN_MAX30105_H_ */