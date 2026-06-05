// ============================================================
// light_sensor.c
// J.A.R.V.I.S. AR 智能眼镜 —— 光照传感器驱动模块
// ============================================================
// 硬件：TEMT6000 环境光传感器模块
// 接线：TEMT6000 VCC → 3.3V，GND → GND，OUT → GPIO 4
//
// 工作模式：初始化成功后，自动创建一个 FreeRTOS 后台任务，
//           以 100ms 为周期持续采样 ADC 并更新全局缓存值。
//           其他模块随时调用 light_sensor_get_lux() 即可获取
//           最新的光照数据，无需自己管理 ADC 读取。
// ============================================================

#include "light_sensor.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LIGHT_SENSOR";

// ============================================================
//   硬件配置常量
// ============================================================
#define LIGHT_ADC_UNIT      ADC_UNIT_1      // 使用 ADC1（ADC2 会被 WiFi 占用）
#define LIGHT_ADC_CHANNEL   ADC_CHANNEL_3   // ADC1 通道 3 对应 GPIO 4
#define LIGHT_ADC_ATTEN     ADC_ATTEN_DB_12 // 12dB 衰减，量程 0~3.1V
#define LIGHT_ADC_BITWIDTH  ADC_BITWIDTH_12 // 12 位分辨率，输出 0~4095

// 采集任务配置
#define READ_TASK_PERIOD_MS 1000            // 采样周期（毫秒）
#define READ_TASK_STACK     4096            // 任务栈大小（字节）
#define READ_TASK_PRIORITY  3               // 任务优先级（低于 UI 的 5）
#define SAMPLE_COUNT        16              // 每次采样的平均次数

// ============================================================
//   模块内部静态变量
// ============================================================
static adc_oneshot_unit_handle_t adc_handle = NULL;
static TaskHandle_t read_task_handle = NULL;
static bool is_initialized = false;

// 缓存的最新采样值（由后台任务持续更新）
static volatile int   s_latest_raw  = 0;    // 最新 ADC 原始值
static volatile float s_latest_lux  = 0.0f; // 最新 Lux 值

// ============================================================
//   后台采集任务
// ============================================================
// 每 100ms 执行一次：
//   1. 连续采样 SAMPLE_COUNT 次 ADC 取平均值
//   2. 将平均值换算为 Lux
//   3. 更新全局缓存变量
// 其他模块随时调用 get_lux / get_raw 即可读取最新数据。
static void light_sensor_read_task(void *pvParameter) {
    ESP_LOGI(TAG, "光照采集任务已启动（周期 %dms）", READ_TASK_PERIOD_MS);
    int print_counter = 0; // 用于控制打印频率

    while (1) {
        int sum = 0;
        int raw = 0;

        // 多次采样取平均值，降低 ADC 噪声
        for (int i = 0; i < SAMPLE_COUNT; i++) {
            if (adc_oneshot_read(adc_handle, LIGHT_ADC_CHANNEL, &raw) == ESP_OK) {
                sum += raw;
            }
        }
        float avg_raw = (float)sum / SAMPLE_COUNT;

        // 更新缓存（volatile 保证其他任务能读到最新值）
        s_latest_raw = (int)avg_raw;

        // ADC 值 → 电压 → Lux 换算
        // 12dB 衰减下满量程约 3.1V
        float voltage = avg_raw * 3.1f / 4095.0f;
        // TEMT6000 + 10KΩ 负载：约 1V ≈ 1000 Lux（数据手册典型值）
        // 满量程 3.1V ≈ 3100 Lux
        s_latest_lux = voltage * 1000.0f;

        // 每秒打印一次光照数据到串口监视器
        print_counter++;
        if (print_counter >= 10) {
            print_counter = 0;
            ESP_LOGI(TAG, "Light: %d Lux, ADC: %d", (int)s_latest_lux, s_latest_raw);
        }

        // 延时一个采样周期
        vTaskDelay(pdMS_TO_TICKS(READ_TASK_PERIOD_MS));
    }
}

// ============================================================
//   初始化光照传感器
// ============================================================
// 流程：
//   1. 创建 ADC 单次采样实例
//   2. 配置 ADC 通道（衰减、位宽）
//   3. 启动后台采集任务
esp_err_t light_sensor_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "光照传感器已经初始化过了，跳过");
        return ESP_OK;
    }

    // ---- 第一步：创建 ADC 单次采样实例 ----
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = LIGHT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 单元创建失败: %s", esp_err_to_name(err));
        return err;
    }

    // ---- 第二步：配置 ADC 通道 ----
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = LIGHT_ADC_ATTEN,
        .bitwidth = LIGHT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(adc_handle, LIGHT_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败: %s", esp_err_to_name(err));
        return err;
    }

    // ---- 第三步：启动后台采集任务 ----
    // 绑定到核心 0（与 WiFi/BLE 同核，避免跨核竞争 ADC）
    xTaskCreatePinnedToCore(
        light_sensor_read_task,  // 任务函数
        "light_read",            // 任务名称
        READ_TASK_STACK,         // 栈大小
        NULL,                    // 参数
        READ_TASK_PRIORITY,      // 优先级
        &read_task_handle,       // 任务句柄
        0                        // 绑定到核心 0
    );

    is_initialized = true;
    ESP_LOGI(TAG, "光照传感器初始化完成 (GPIO 4 / ADC1_CH3)");
    return ESP_OK;
}

// ============================================================
//   获取最新光照强度（Lux）
// ============================================================
// 直接返回后台任务缓存的最新值，不阻塞、不执行 ADC 操作。
// 任何任务都可以安全调用（volatile 读取是原子的）。
float light_sensor_get_lux(void) {
    return s_latest_lux;
}

// ============================================================
//   获取最新 ADC 原始值
// ============================================================
int light_sensor_get_raw(void) {
    return s_latest_raw;
}

// ============================================================
//   释放光照传感器资源
// ============================================================
// 停止后台采集任务，释放 ADC 硬件资源。
// 调用后需要重新 init 才能继续使用。
esp_err_t light_sensor_deinit(void) {
    if (!is_initialized) return ESP_OK;

    // 停止后台任务
    if (read_task_handle != NULL) {
        vTaskDelete(read_task_handle);
        read_task_handle = NULL;
    }

    // 释放 ADC
    if (adc_handle != NULL) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    is_initialized = false;
    s_latest_raw = 0;
    s_latest_lux = 0.0f;

    ESP_LOGI(TAG, "光照传感器已释放");
    return ESP_OK;
}
