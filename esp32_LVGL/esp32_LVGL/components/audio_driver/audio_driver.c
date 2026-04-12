#include "audio_driver.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 定义日志标签，方便在串口中过滤信息
static const char *TAG = "AUDIO_DRIVER";

// 绑定到 ESP32-S3 SuperMini 的可用安全引脚
#define I2S_BCLK_PIN  GPIO_NUM_6
#define I2S_WS_PIN    GPIO_NUM_5
#define I2S_DOUT_PIN  GPIO_NUM_7

// 静态全局变量：保存 I2S 发送通道句柄，模块外无法直接访问
static i2s_chan_handle_t tx_chan = NULL;

esp_err_t audio_driver_init(void) {
    // 1. 防止重复初始化
    if (tx_chan != NULL) {
        ESP_LOGW(TAG, "Audio driver is already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing I2S audio driver...");

    // 2. 分配 I2S 通道 (自动选择空闲的 I2S 端口，配置为主机模式)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel");
        return err;
    }

    // 3. 配置 I2S 标准模式参数 (采样率: 16000Hz, 格式: 16-bit 单声道)
    i2s_std_config_t std_cfg = {
        // 💡 修改点 1：采样率改为 16000
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), 
        
        // 💡 修改点 2：声道模式从 STEREO 改为 MONO (单声道)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), 
        
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode");
        // 如果初始化失败，清理已分配的通道避免内存泄漏
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return err;
    }

    // 4. 启用 I2S 通道
    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel");
        return err;
    }

    ESP_LOGI(TAG, "I2S audio driver initialized successfully. Pins - WS: %d, BCLK: %d, DOUT: %d", 
             I2S_WS_PIN, I2S_BCLK_PIN, I2S_DOUT_PIN);
    
    return ESP_OK;
}

esp_err_t audio_driver_play(const void *audio_data, size_t len) {
    // 检查是否已经初始化
    if (tx_chan == NULL) {
        ESP_LOGE(TAG, "Cannot play: I2S channel not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_written = 0;
    // 使用 portMAX_DELAY 阻塞等待，确保数据全部送入 DMA 缓冲区
    esp_err_t err = i2s_channel_write(tx_chan, audio_data, len, &bytes_written, portMAX_DELAY);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write audio data to I2S buffer");
    }
    
    return err;
}