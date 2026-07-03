#include "bmp280.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui_globals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>

static const char *TAG = "BMP280";

#define BMP280_REG_CHIPID       0xD0
#define BMP280_REG_RESET        0xE0
#define BMP280_REG_CTRL_MEAS    0xF4
#define BMP280_REG_CONFIG       0xF5
#define BMP280_REG_TEMP_MSB     0xFA  // 温度寄存器起始地址

// 只保存温度校准数据
static struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
} calib_data;

static int32_t t_fine;

static esp_err_t write_register(i2c_port_t i2c_num, uint8_t reg_addr, uint8_t data) {
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(i2c_num, BMP280_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(1000));
}

static esp_err_t read_registers(i2c_port_t i2c_num, uint8_t reg_addr, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(i2c_num, BMP280_I2C_ADDR, &reg_addr, 1, data, len, pdMS_TO_TICKS(1000));
}

esp_err_t bmp280_init(i2c_port_t i2c_num) {
    uint8_t chip_id = 0;

    if (i2c_mutex) xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(500));

    if (read_registers(i2c_num, BMP280_REG_CHIPID, &chip_id, 1) != ESP_OK || chip_id != 0x58) {
        ESP_LOGE(TAG, "BMP280 ID 错误: 0x%02X", chip_id);
        if (i2c_mutex) xSemaphoreGive(i2c_mutex);
        return ESP_FAIL;
    }

    write_register(i2c_num, BMP280_REG_RESET, 0xB6);
    if (i2c_mutex) xSemaphoreGive(i2c_mutex);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (i2c_mutex) xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(500));

    // 只读温度校准数据（6 字节：dig_T1, dig_T2, dig_T3）
    uint8_t calib[6];
    read_registers(i2c_num, 0x88, calib, 6);
    calib_data.dig_T1 = (calib[1] << 8) | calib[0];
    calib_data.dig_T2 = (calib[3] << 8) | calib[2];
    calib_data.dig_T3 = (calib[5] << 8) | calib[4];

    // 先写 CONFIG，后写 CTRL_MEAS
    write_register(i2c_num, BMP280_REG_CONFIG, 0xA0);
    write_register(i2c_num, BMP280_REG_CTRL_MEAS, 0x57);

    if (i2c_mutex) xSemaphoreGive(i2c_mutex);

    ESP_LOGI(TAG, "BMP280 初始化成功!");
    return ESP_OK;
}

// 只读温度（3 字节）
esp_err_t bmp280_read_temp(i2c_port_t i2c_num, float *temperature) {
    uint8_t raw[3];

    if (i2c_mutex && xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_FAIL;
    }

    esp_err_t err = read_registers(i2c_num, BMP280_REG_TEMP_MSB, raw, 3);

    if (i2c_mutex) xSemaphoreGive(i2c_mutex);

    if (err != ESP_OK) return ESP_FAIL;

    int32_t adc_T = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);

    int32_t v1 = ((((adc_T >> 3) - ((int32_t)calib_data.dig_T1 << 1))) * ((int32_t)calib_data.dig_T2)) >> 11;
    int32_t v2 = (((((adc_T >> 4) - ((int32_t)calib_data.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib_data.dig_T1))) >> 12) * ((int32_t)calib_data.dig_T3)) >> 14;
    t_fine = v1 + v2;
    *temperature = ((t_fine * 5 + 128) >> 8) / 100.0f;

    return ESP_OK;
}

void read_bmp280_task(void *pvParameters) {
    float temp;
    char temp_str[16];
    int fail_count = 0;

    while (1) {
        esp_err_t err = bmp280_read_temp(I2C_NUM_0, &temp);
        if (err == ESP_OK) {
            fail_count = 0;
            ESP_LOGI(TAG, "Temp: %.1f C", temp);
            snprintf(temp_str, sizeof(temp_str), "%.1f C", temp);

            if (lvgl_port_lock(0)) {
                if (label_temp != NULL) {
                    lv_label_set_text(label_temp, temp_str);
                }
                lvgl_port_unlock();
            }
        } else {
            fail_count++;
            if (fail_count <= 3 || fail_count % 10 == 0) {
                ESP_LOGW(TAG, "读取失败 #%d", fail_count);
            }
            if (fail_count > 10) {
                ESP_LOGE(TAG, "连续失败，软复位 BMP280");
                bmp280_init(I2C_NUM_0);
                fail_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
