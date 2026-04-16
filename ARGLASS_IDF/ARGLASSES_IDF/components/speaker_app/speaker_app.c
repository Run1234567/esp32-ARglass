#include "speaker_app.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

static const char *TAG = "SPEAKER_APP";

// ? 必须加上下面这两行，编译器才能识别 portMAX_DELAY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 引脚定义
#define SPK_I2S_WS   1
#define SPK_I2S_BCK  2
#define SPK_I2S_DATA 3

static i2s_chan_handle_t tx_chan;

void initSpeaker(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // 防结巴关键
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), // 单声道
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_I2S_BCK,
            .ws   = SPK_I2S_WS,
            .dout = SPK_I2S_DATA,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

// ? 确保这个函数存在，且参数和 main.c 调用时一致
void playSpeaker(const uint8_t *data, size_t length) {
    size_t bytes_written = 0;
    i2s_channel_write(tx_chan, data, length, &bytes_written, portMAX_DELAY);
}