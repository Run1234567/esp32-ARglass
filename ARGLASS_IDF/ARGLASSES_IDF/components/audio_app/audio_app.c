#include "audio_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 引入 ESP-IDF v5 专用的 PDM I2S 驱动
#include "driver/i2s_pdm.h" 

static const char *TAG = "AUDIO_APP";

// ==========================================
// ? 麦克风引脚定义 (根据你的配置)
// ==========================================
#define I2S_WS_IO   42  // PDM 的时钟输出引脚
#define I2S_SD_IO   41  // PDM 的数据输入引脚

// I2S 接收通道句柄 (全局变量，保存通道实例)
static i2s_chan_handle_t rx_chan; 

void initAudio(void) {
    ESP_LOGI(TAG, "Initializing PDM Microphone...");

    // 1. 分配 I2S 通道 (自动选择空闲的 I2S 端口，设定为主机模式)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    // 2. 配置 PDM RX 模式的参数
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        // 采样率设为 16000Hz
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        // 16bit 采样深度，单声道模式 (Mono)
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        // 引脚映射配置
        .gpio_cfg = {
            .clk = I2S_WS_IO,
            .din = I2S_SD_IO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    // 3. 将配置应用到通道，并启动通道
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "? 麦克风 I2S PDM 模式初始化成功！");
}

size_t readAudio(int16_t* buffer, size_t samples) {
    size_t bytes_read = 0;
    
    // 从通道读取数据，给予 10ms 的超时时间防卡死
    esp_err_t ret = i2s_channel_read(rx_chan, buffer, samples * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(10));
    
    if (ret != ESP_OK) {
        // 如果超时或出错，可以在这里打印日志（高频读取时建议注释掉错误日志防刷屏）
        // ESP_LOGW(TAG, "Audio read timeout or error");
    }
    
    return bytes_read;
}