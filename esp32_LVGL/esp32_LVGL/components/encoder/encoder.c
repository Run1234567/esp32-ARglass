/**
 * @file encoder.c
 * @brief 旋转编码器驱动 —— 使用 PCNT 硬件计数器 (ESP-IDF 6.x 新API)
 *
 * 硬件接线：
 *   编码器 A相 → GPIO 47
 *   编码器 B相 → GPIO 48
 *   VCC → 3.3V，GND → GND
 */

#include "encoder.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

static const char *TAG = "ENCODER";

#define ENCODER_GPIO_A  47
#define ENCODER_GPIO_B  48

static pcnt_unit_handle_t pcnt_unit = NULL;

esp_err_t encoder_init(void) {
    // 1. 创建 PCNT 单元
    pcnt_unit_config_t unit_config = {
        .high_limit = 32767,
        .low_limit = -32768,
    };
    esp_err_t err = pcnt_new_unit(&unit_config, &pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT 单元创建失败: %s", esp_err_to_name(err));
        return err;
    }

    // 2. 配置 A相 通道（脉冲输入）
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = ENCODER_GPIO_A,   // A相上升沿/下降沿
        .level_gpio_num = ENCODER_GPIO_B,  // B相用于判断方向
    };
    pcnt_channel_handle_t pcnt_chan_a = NULL;
    err = pcnt_new_channel(pcnt_unit, &chan_a_config, &pcnt_chan_a);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT A相通道创建失败: %s", esp_err_to_name(err));
        return err;
    }

    // 3. 配置计数规则：A相上升沿+1，下降沿-1
    pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);

    // 4. 配置方向控制：B相高电平时反转计数方向
    pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // 5. 启用 PCNT
    err = pcnt_unit_enable(pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT 启用失败: %s", esp_err_to_name(err));
        return err;
    }

    // 6. 开始计数
    err = pcnt_unit_start(pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCNT 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "编码器初始化完成 (A:GPIO%d B:GPIO%d)", ENCODER_GPIO_A, ENCODER_GPIO_B);
    return ESP_OK;
}

int32_t encoder_get_count(void) {
    int count = 0;
    if (pcnt_unit != NULL) {
        pcnt_unit_get_count(pcnt_unit, &count);
    }
    return (int32_t)count;
}

void encoder_reset(void) {
    if (pcnt_unit != NULL) {
        pcnt_unit_clear_count(pcnt_unit);
    }
}
