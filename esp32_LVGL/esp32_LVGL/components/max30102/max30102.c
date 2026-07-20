// ============================================================
// max30102.c
// J.A.R.V.I.S. AR 智能眼镜 —— MAX30102 心率血氧传感器驱动
// ============================================================
// 使用旧版 i2c driver（与 MPU6050/BMP280 共享 I2C_NUM_0）
// 引脚：GPIO 1 (SCL), GPIO 2 (SDA)
// ============================================================

#include "max30102.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "MAX30102";

// ---- 寄存器地址 ----
#define MAX30102_ADDR       0x57
#define REG_FIFO_DATA       0x07
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C
#define REG_LED2_PA         0x0D

// ---- 使用独立 I2C 总线（GPIO 38/39） ----
#define I2C_PORT            I2C_NUM_1
#define MAX30102_SCL        38
#define MAX30102_SDA        39
#define MAX30102_I2C_FREQ   100000

// ---- 任务配置 ----
#define HR_TASK_STACK       4096
#define HR_TASK_PRIO        5

// ---- 模块内部变量 ----
static TaskHandle_t hr_task_handle = NULL;
static bool is_initialized = false;

static volatile float s_bpm  = 0.0f;
static volatile float s_spo2 = 0.0f;

// ============================================================
//   I2C 读写（旧版 API，与其他传感器共享总线）
// ============================================================
static esp_err_t max30102_write_reg(uint8_t reg, uint8_t data) {
    uint8_t write_buf[2] = {reg, data};
    return i2c_master_write_to_device(I2C_PORT, MAX30102_ADDR, write_buf, 2, 1000 / portTICK_PERIOD_MS);
}

static esp_err_t max30102_read_fifo(uint8_t *buffer, size_t size) {
    uint8_t reg = REG_FIFO_DATA;
    return i2c_master_write_read_device(I2C_PORT, MAX30102_ADDR, &reg, 1, buffer, size, 1000 / portTICK_PERIOD_MS);
}

// ============================================================
//   后台心率血氧任务（算法不变）
// ============================================================
static void heart_rate_task(void *pvParameters) {
    uint8_t data_buf[6];

    float dc_ir = 0, ac_ir = 0, last_ac_ir = 0;
    float dc_red = 0, ac_red = 0;
    int64_t last_beat_time = 0;
    float bpm_history[10] = {0};
    int bpm_idx = 0;
    static int print_count = 0;

    while (1) {
        if (max30102_read_fifo(data_buf, 6) == ESP_OK) {
            uint32_t red_raw = ((data_buf[0] << 16) | (data_buf[1] << 8) | data_buf[2]) & 0x03FFFF;
            uint32_t ir_raw  = ((data_buf[3] << 16) | (data_buf[4] << 8) | data_buf[5]) & 0x03FFFF;

            // 每 500 次（约5秒）打印一次原始数据
            if (++print_count >= 500) {
                ESP_LOGI(TAG, "原始数据 | RED: %lu | IR: %lu", red_raw, ir_raw);
                print_count = 0;
            }

            if (ir_raw > 30000) {
                dc_ir = 0.95f * dc_ir + 0.05f * (float)ir_raw;
                dc_red = 0.95f * dc_red + 0.05f * (float)red_raw;
                ac_ir = (float)ir_raw - dc_ir;
                ac_red = (float)red_raw - dc_red;

                if (last_ac_ir > 0 && ac_ir <= 0 && last_ac_ir > 20.0f) {
                    int64_t current_time = esp_timer_get_time();
                    float delta_sec = (current_time - last_beat_time) / 1000000.0f;

                    if (delta_sec >= 0.3f && delta_sec <= 1.5f) {
                        float bpm = 60.0f / delta_sec;
                        bpm_history[bpm_idx] = bpm;
                        bpm_idx = (bpm_idx + 1) % 10;

                        float sum = 0;
                        for (int i = 0; i < 10; i++) sum += bpm_history[i];
                        s_bpm = sum / 10.0f;

                        float ratio = (ac_red / dc_red) / (ac_ir / dc_ir);
                        float spo2 = 110.0f - (25.0f * ratio);
                        if (spo2 > 100) spo2 = 99.0f;
                        if (spo2 < 85) spo2 = 90.0f;
                        s_spo2 = spo2;
                        ESP_LOGI(TAG, "心率: %.1f BPM | 血氧: %.1f%%", s_bpm, s_spo2);
                    }
                    last_beat_time = current_time;
                }
                last_ac_ir = ac_ir;
            } else {
                ESP_LOGW(TAG, "请将手指贴紧传感器...");
            }
        } else {
            ESP_LOGE(TAG, "I2C 读取失败，等待总线稳定...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            // 重新配置传感器
            max30102_write_reg(REG_MODE_CONFIG, 0x03);
            max30102_write_reg(REG_LED1_PA, 0x50);
            max30102_write_reg(REG_LED2_PA, 0x50);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
//   初始化（传感器配置，不创建 I2C 总线）
// ============================================================
esp_err_t max30102_init(void) {
    if (is_initialized) return ESP_OK;

    // 0. 初始化独立 I2C 总线（GPIO 38/39）
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)MAX30102_SDA;
    conf.scl_io_num = (gpio_num_t)MAX30102_SCL;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = MAX30102_I2C_FREQ;
    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 配置失败: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MAX30102 I2C 初始化完成 (SCL:%d SDA:%d)", MAX30102_SCL, MAX30102_SDA);

    // 1. 硬件复位
    max30102_write_reg(REG_MODE_CONFIG, 0x40);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. 配置模式为 SpO2 + Heart Rate
    max30102_write_reg(REG_MODE_CONFIG, 0x03);

    // 3. 增强 LED 驱动电流（0x50 比默认 0x30 更亮，信号更稳定）
    max30102_write_reg(REG_LED1_PA, 0x50);
    max30102_write_reg(REG_LED2_PA, 0x50);

    // 4. 配置采样率和脉宽
    max30102_write_reg(REG_SPO2_CONFIG, 0x27);

    // 5. 清空 FIFO
    max30102_write_reg(0x04, 0x00);
    max30102_write_reg(0x05, 0x00);
    max30102_write_reg(0x06, 0x00);

    is_initialized = true;
    ESP_LOGI(TAG, "MAX30102 初始化完成");
    return ESP_OK;
}

// ============================================================
//   启动后台任务（进入健康界面时调用）
// ============================================================
esp_err_t max30102_start_task(void) {
    if (!is_initialized) return ESP_ERR_INVALID_STATE;
    if (hr_task_handle != NULL) return ESP_OK; // 已在运行

    // 重新配置传感器（防止之前被关闭）
    max30102_write_reg(REG_MODE_CONFIG, 0x03);
    max30102_write_reg(REG_LED1_PA, 0x50);
    max30102_write_reg(REG_LED2_PA, 0x50);
    max30102_write_reg(REG_SPO2_CONFIG, 0x27);
    max30102_write_reg(0x04, 0x00);
    max30102_write_reg(0x05, 0x00);
    max30102_write_reg(0x06, 0x00);

    // 重置数据
    s_bpm = 0.0f;
    s_spo2 = 0.0f;

    // 栈放 PSRAM
    StackType_t *hr_stack = heap_caps_malloc(HR_TASK_STACK, MALLOC_CAP_SPIRAM);
    StaticTask_t *hr_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (hr_stack && hr_tcb) {
        hr_task_handle = xTaskCreateStaticPinnedToCore(heart_rate_task, "hr_task",
                            HR_TASK_STACK/sizeof(StackType_t), NULL, HR_TASK_PRIO,
                            hr_stack, hr_tcb, 0);
    }
    ESP_LOGI(TAG, "心率采集任务已启动");
    return ESP_OK;
}

// ============================================================
//   停止后台任务（退出健康界面时调用）
// ============================================================
esp_err_t max30102_stop_task(void) {
    if (hr_task_handle != NULL) {
        vTaskDelete(hr_task_handle);
        hr_task_handle = NULL;
    }
    // 关闭 LED 节省功耗
    max30102_write_reg(REG_LED1_PA, 0x00);
    max30102_write_reg(REG_LED2_PA, 0x00);
    s_bpm = 0.0f;
    s_spo2 = 0.0f;
    ESP_LOGI(TAG, "心率采集任务已停止");
    return ESP_OK;
}

// ============================================================
//   获取数据
// ============================================================
float max30102_get_bpm(void)  { return s_bpm; }
float max30102_get_spo2(void) { return s_spo2; }

// ============================================================
//   释放
// ============================================================
esp_err_t max30102_deinit(void) {
    if (!is_initialized) return ESP_OK;
    if (hr_task_handle) { vTaskDelete(hr_task_handle); hr_task_handle = NULL; }
    is_initialized = false;
    return ESP_OK;
}
