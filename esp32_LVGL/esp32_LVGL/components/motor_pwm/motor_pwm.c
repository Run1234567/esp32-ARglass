/**
 * @file motor_pwm.c
 * @brief 震动马达 PWM 驱动 —— 使用 LEDC 硬件 PWM
 *
 * 硬件接线：
 *   马达驱动信号 → GPIO 14
 *   VCC → 5V/3.3V，GND → GND
 */

#include "motor_pwm.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "MOTOR";

// 硬件参数配置
#define MOTOR_PWM_TIMER         LEDC_TIMER_0
#define MOTOR_PWM_MODE          LEDC_LOW_SPEED_MODE
#define MOTOR_PWM_GPIO          14
#define MOTOR_PWM_CHANNEL       LEDC_CHANNEL_0
#define MOTOR_PWM_DUTY_RES      LEDC_TIMER_10_BIT
#define MOTOR_PWM_FREQ_HZ       5000

void motor_pwm_init(void) {
    // 1. 配置 LEDC 定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = MOTOR_PWM_MODE,
        .timer_num        = MOTOR_PWM_TIMER,
        .duty_resolution  = MOTOR_PWM_DUTY_RES,
        .freq_hz          = MOTOR_PWM_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 2. 配置 LEDC 通道
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = MOTOR_PWM_MODE,
        .channel        = MOTOR_PWM_CHANNEL,
        .timer_sel      = MOTOR_PWM_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = MOTOR_PWM_GPIO,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "震动马达 PWM 初始化完成 (GPIO %d)", MOTOR_PWM_GPIO);
}

void motor_set_vibration(uint8_t percentage) {
    if (percentage > 100) percentage = 100;

    // 0-100% 映射到 0-1023
    uint32_t duty = (percentage * 1023) / 100;

    ledc_set_duty(MOTOR_PWM_MODE, MOTOR_PWM_CHANNEL, duty);
    ledc_update_duty(MOTOR_PWM_MODE, MOTOR_PWM_CHANNEL);
}

void motor_pulse_short(void) {
    // 60% 强度，80ms 短促震动
    motor_set_vibration(60);
    vTaskDelay(pdMS_TO_TICKS(80));
    motor_set_vibration(0);
}
