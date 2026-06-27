#include "bmp280.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui_globals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "BMP280";

#define BMP280_REG_CHIPID       0xD0
#define BMP280_REG_RESET        0xE0
#define BMP280_REG_CTRL_MEAS    0xF4
#define BMP280_REG_CONFIG       0xF5
#define BMP280_REG_PRESS_MSB    0xF7

static struct {
    uint16_t dig_T1; int16_t  dig_T2; int16_t  dig_T3;
    uint16_t dig_P1; int16_t  dig_P2; int16_t  dig_P3;
    int16_t  dig_P4; int16_t  dig_P5; int16_t  dig_P6;
    int16_t  dig_P7; int16_t  dig_P8; int16_t  dig_P9;
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
    if (read_registers(i2c_num, BMP280_REG_CHIPID, &chip_id, 1) != ESP_OK || chip_id != 0x58) {
        ESP_LOGE(TAG, "BMP280 ID ����: 0x%02X", chip_id);
        return ESP_FAIL;
    }

    write_register(i2c_num, BMP280_REG_RESET, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t calib[24];
    read_registers(i2c_num, 0x88, calib, 24);
    calib_data.dig_T1 = (calib[1] << 8) | calib[0];
    calib_data.dig_T2 = (calib[3] << 8) | calib[2];
    calib_data.dig_T3 = (calib[5] << 8) | calib[4];
    calib_data.dig_P1 = (calib[7] << 8) | calib[6];
    calib_data.dig_P2 = (calib[9] << 8) | calib[8];
    calib_data.dig_P3 = (calib[11] << 8) | calib[10];
    calib_data.dig_P4 = (calib[13] << 8) | calib[12];
    calib_data.dig_P5 = (calib[15] << 8) | calib[14];
    calib_data.dig_P6 = (calib[17] << 8) | calib[16];
    calib_data.dig_P7 = (calib[19] << 8) | calib[18];
    calib_data.dig_P8 = (calib[21] << 8) | calib[20];
    calib_data.dig_P9 = (calib[23] << 8) | calib[22];

    write_register(i2c_num, BMP280_REG_CTRL_MEAS, 0x57);
    write_register(i2c_num, BMP280_REG_CONFIG, 0xA0);

    ESP_LOGI(TAG, "BMP280 ��ʼ���ɹ�!");
    return ESP_OK;
}

esp_err_t bmp280_read_data(i2c_port_t i2c_num, float *temperature, float *pressure) {
    uint8_t raw[6];
    if (read_registers(i2c_num, BMP280_REG_PRESS_MSB, raw, 6) != ESP_OK) return ESP_FAIL;

    int32_t adc_P = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
    int32_t adc_T = (raw[3] << 12) | (raw[4] << 4) | (raw[5] >> 4);

    // �¶Ȳ���
    int32_t v1 = ((((adc_T >> 3) - ((int32_t)calib_data.dig_T1 << 1))) * ((int32_t)calib_data.dig_T2)) >> 11;
    int32_t v2 = (((((adc_T >> 4) - ((int32_t)calib_data.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib_data.dig_T1))) >> 12) * ((int32_t)calib_data.dig_T3)) >> 14;
    t_fine = v1 + v2;
    *temperature = ((t_fine * 5 + 128) >> 8) / 100.0f;

    // ��ѹ����
    int64_t p_v1, p_v2, p;
    p_v1 = ((int64_t)t_fine) - 128000;
    p_v2 = p_v1 * p_v1 * (int64_t)calib_data.dig_P6;
    p_v2 = p_v2 + ((p_v1 * (int64_t)calib_data.dig_P5) << 17);
    p_v2 = p_v2 + (((int64_t)calib_data.dig_P4) << 35);
    p_v1 = ((p_v1 * p_v1 * (int64_t)calib_data.dig_P3) >> 8) + ((p_v1 * (int64_t)calib_data.dig_P2) << 12);
    p_v1 = (((((int64_t)1) << 47) + p_v1)) * ((int64_t)calib_data.dig_P1) >> 33;
    if (p_v1 == 0) return ESP_FAIL;
    p = 1048576 - adc_P;
    p = (((p << 31) - p_v2) * 3125) / p_v1;
    p_v1 = (((int64_t)calib_data.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    p_v2 = (((int64_t)calib_data.dig_P8) * p) >> 19;
    *pressure = ((p + p_v1 + p_v2) >> 8) / 256.0f + (((int64_t)calib_data.dig_P7) << 4) / 256.0f;

    return ESP_OK;
}

void read_bmp280_task(void *pvParameters) {
    float temp, press;
    char temp_str[16];

    while (1) {
        if (bmp280_read_data(I2C_NUM_0, &temp, &press) == ESP_OK) {
            ESP_LOGI(TAG, "Temp: %.2f C, Press: %.2f Pa", temp, press);

            // 用 snprintf 格式化（LVGL 不支持 %f）
            snprintf(temp_str, sizeof(temp_str), "%.1f C", temp);

            // 加锁更新 UI
            if (lvgl_port_lock(0)) {
                if (label_temp != NULL) {
                    lv_label_set_text(label_temp, temp_str);
                }
                lvgl_port_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}