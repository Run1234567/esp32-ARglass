#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h" // 提供高精度微秒延时 esp_rom_delay_us()

static const char *TAG = "TOUCH_WHEEL";

#define CHARGE_PIN    GPIO_NUM_9
#define TOUCH_GATE    30   // 触摸判定阈值 (根据实际情况修改)

// ★ 充放电延时参数，确保校准和主循环一致
#define DISCHARGE_US  200  // 放电等待时间 (us)
#define CHARGE_US     1    // 充电采样相位点 (us)，根据实际灵敏度微调

// 定义 5 个通道: UP(GPIO8) DOWN(GPIO5) LEFT(GPIO7) MID(GPIO6) RIGHT(GPIO4)
adc_channel_t touch_channels[5] = {
    ADC_CHANNEL_7, // GPIO 8 (UP   上)
    ADC_CHANNEL_4, // GPIO 5 (DOWN 下)
    ADC_CHANNEL_6, // GPIO 7 (LEFT 左)
    ADC_CHANNEL_5, // GPIO 6 (MID  中)
    ADC_CHANNEL_3, // GPIO 4 (RIGHT 右)
};

// 核心数据结构 (复刻原版)
float CapFilter[5] = {0};   // 滤波值 (用浮点数更平滑)
int16_t Touch_Vref[5] = {0}; // 开机基准值
int16_t Touch_Diff[5] = {0}; // 差值 (按下力度)

void touch_wheel_task(void *pvParameters) {
    // 1. 初始化 ADC
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    for (int i = 0; i < 5; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, touch_channels[i], &config));
    }

    // ★ 强制关闭 ADC 引脚内部上拉/下拉，保证极板绝对不漏电
    gpio_num_t adc_pins[5] = {GPIO_NUM_8, GPIO_NUM_5, GPIO_NUM_7, GPIO_NUM_6, GPIO_NUM_4};
    for (int i = 0; i < 5; i++) {
        gpio_set_pull_mode(adc_pins[i], GPIO_FLOATING);
    }

    // 2. 初始化激励引脚
    gpio_set_direction(CHARGE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CHARGE_PIN, 0);

    // ==========================================
    // 阶段 A：开机动态校准 (Touch_Init)
    // ==========================================
    ESP_LOGI(TAG, "正在进行环境基准校准，请勿触摸板子！...");
    vTaskDelay(pdMS_TO_TICKS(500)); // 等待系统稳定

    for (int init_loop = 0; init_loop < 100; init_loop++) {
        for (int i = 0; i < 5; i++) {
            // 彻底放电 (输出拉低)
            gpio_set_direction(CHARGE_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(CHARGE_PIN, 0);
            esp_rom_delay_us(DISCHARGE_US);

            // 瞬间充电 (拉高)
            gpio_set_level(CHARGE_PIN, 1);
            esp_rom_delay_us(CHARGE_US);

            // ★ 切断电源，把电荷"憋"在极板上
            gpio_set_direction(CHARGE_PIN, GPIO_MODE_INPUT);

            // 慢悠悠读 ADC，电压已被锁住
            int raw_val = 0;
            adc_oneshot_read(adc1_handle, touch_channels[i], &raw_val);

            // 预热滤波
            if (init_loop == 0) CapFilter[i] = raw_val;
            else CapFilter[i] = 0.05 * raw_val + 0.95 * CapFilter[i];
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // 记录基准值 (加5防止轻微底噪误触发)
    for (int i = 0; i < 5; i++) {
        Touch_Vref[i] = (int16_t)CapFilter[i] + 5;
        ESP_LOGI(TAG, "通道 %d 基准值锁定: %d", i, Touch_Vref[i]);
    }
    ESP_LOGI(TAG, "校准完成，开始监测！");

    // ==========================================
    // 阶段 B：主循环，高频扫描与滤波
    // ==========================================
    while (1) {
        for (int i = 0; i < 5; i++) {
            // 彻底放电 (输出拉低)
            gpio_set_direction(CHARGE_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(CHARGE_PIN, 0);
            esp_rom_delay_us(DISCHARGE_US);

            // 瞬间充电 (拉高)
            gpio_set_level(CHARGE_PIN, 1);
            esp_rom_delay_us(CHARGE_US);

            // ★ 切断电源，把电荷"憋"在极板上
            gpio_set_direction(CHARGE_PIN, GPIO_MODE_INPUT);

            // 慢悠悠读 ADC，电压已被锁住
            int raw_val = 0;
            adc_oneshot_read(adc1_handle, touch_channels[i], &raw_val);

            // 一阶低通滤波 (复刻原版核心公式)
            CapFilter[i] = 0.05 * raw_val + 0.95 * CapFilter[i];

            // 计算触摸力度 (基准值 - 当前值)
            Touch_Diff[i] = Touch_Vref[i] - (int16_t)CapFilter[i];

            // 如果差值小于0说明是底噪漂移，归零
            if (Touch_Diff[i] < 0) Touch_Diff[i] = 0;
        }

        // --- 打印测试结果，方便你调整阈值 ---
        // 为了不刷屏，只要有按键超过阈值才打印
        if (Touch_Diff[0] > TOUCH_GATE || Touch_Diff[1] > TOUCH_GATE ||
            Touch_Diff[2] > TOUCH_GATE || Touch_Diff[3] > TOUCH_GATE ||
            Touch_Diff[4] > TOUCH_GATE) {

            ESP_LOGI(TAG, "UP: %3d | DN: %3d | LT: %3d | MID: %3d | RT: %3d",
                     Touch_Diff[0], Touch_Diff[1], Touch_Diff[2], Touch_Diff[3], Touch_Diff[4]);
        }

        // 这里扫描一圈大概需要 1~2 毫秒，直接加个小延时让出 CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    xTaskCreate(touch_wheel_task, "touch_wheel", 4096, NULL, 5, NULL);
}
