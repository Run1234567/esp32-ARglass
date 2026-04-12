#include "max30105.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAX30105";

// 辅助函数：向寄存器写入 1 字节数据
static esp_err_t write_register(i2c_port_t i2c_num, uint8_t reg_addr, uint8_t data) {
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(i2c_num, MAX30105_I2C_ADDR, write_buf, 2, 1000 / portTICK_PERIOD_MS);
}

// 辅助函数：从寄存器读取多字节数据
static esp_err_t read_registers(i2c_port_t i2c_num, uint8_t reg_addr, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(i2c_num, MAX30105_I2C_ADDR, &reg_addr, 1, data, len, 1000 / portTICK_PERIOD_MS);
}

esp_err_t max30105_init(i2c_port_t i2c_num) {
    esp_err_t err;

    // 1. 软复位传感器
    err = write_register(i2c_num, MAX30105_REG_MODE_CONFIG, 0x40);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待复位完成

    // 2. 设置为 SpO2 模式 (开启 Red 和 IR LED)
    err = write_register(i2c_num, MAX30105_REG_MODE_CONFIG, 0x03);
    
    // 3. 配置 SpO2 寄存器: ADC范围=4096nA, 采样率=400Hz, LED脉宽=411us
    err |= write_register(i2c_num, MAX30105_REG_SPO2_CONFIG, 0x27);

    // 4. 设置 LED 亮度 (0x00 到 0xFF, 0x24 大概是 7mA, 足够测试用)
    err |= write_register(i2c_num, MAX30105_REG_LED1_PA, 0x24); // Red
    err |= write_register(i2c_num, MAX30105_REG_LED2_PA, 0x24); // IR

    // 5. 清空 FIFO 指针，准备读取
    err |= write_register(i2c_num, MAX30105_REG_FIFO_WR_PTR, 0x00);
    err |= write_register(i2c_num, MAX30105_REG_OVF_COUNTER, 0x00);
    err |= write_register(i2c_num, MAX30105_REG_FIFO_RD_PTR, 0x00);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "MAX30105 初始化成功!");
    } else {
        ESP_LOGE(TAG, "MAX30105 初始化失败!");
    }
    return err;
}

esp_err_t max30105_read_fifo(i2c_port_t i2c_num, uint32_t *red, uint32_t *ir) {
    uint8_t buffer[6]; // SpO2 模式下，每个样本是 6 字节 (Red 3字节 + IR 3字节)
    
    // 从 FIFO 数据寄存器读取 6 个字节
    esp_err_t err = read_registers(i2c_num, MAX30105_REG_FIFO_DATA, buffer, 6);
    if (err != ESP_OK) return err;

    // MAX30105 的数据是 18 位的，需要将 3 个字节拼接起来并屏蔽掉高位垃圾数据
    *red = ((buffer[0] << 16) | (buffer[1] << 8) | buffer[2]) & 0x03FFFF;
    *ir  = ((buffer[3] << 16) | (buffer[4] << 8) | buffer[5]) & 0x03FFFF;

    return ESP_OK;
}