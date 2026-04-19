#ifndef BMP280_H
#define BMP280_H

#include <stdint.h>
#include "driver/i2c.h"
#include "esp_err.h"

#define BMP280_I2C_ADDR             0x76 

esp_err_t bmp280_init(i2c_port_t i2c_num);
esp_err_t bmp280_read_data(i2c_port_t i2c_num, float *temperature, float *pressure);
void read_bmp280_task(void *pvParameters);

#endif