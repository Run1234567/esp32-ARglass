#include "speaker_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// 引入 ESP-IDF v5 专用的标准 I2S (TX/RX) 驱动
#include "driver/i2s_std.h"

static const char *TAG = "SPEAKER_APP";

// ==========================================
// ? 喇叭引脚定义 (基于你的黄金引脚配置)
// ==========================================
#define SPK_I2S_WS   1
#define SPK_I2S_BCK  2
#define SPK_I2S_DATA 3

// I2S 发送通道句柄
static i2s_chan_handle_t tx_chan;

void initSpeaker(void) {
    ESP_LOGI(TAG, "Initializing Speaker I2S...");

    // 1. 分配 I2S 通道 (自动选择空闲端口，设为主机模式)
    // 注意：第二个参数是 tx_chan 的指针，第三个是 rx_chan (设为 NULL 因为喇叭只发不收)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    // 2. 配置标准 I2S 模式参数
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), // 统一设为 16kHz
        // 16-bit 采样，立体声(Stereo)模式兼容性最好
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // 喇叭不需要 MCLK
            .bclk = SPK_I2S_BCK,
            .ws   = SPK_I2S_WS,
            .dout = SPK_I2S_DATA,
            .din  = I2S_GPIO_UNUSED, // 喇叭不需要输入
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // 3. 将配置应用到通道，并启动
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    ESP_LOGI(TAG, "? 喇叭 I2S 标准发送通道初始化完毕！");
}

void playSpeaker(const uint8_t *data, size_t length) {
    size_t bytes_written = 0;
    
    // 将二进制音频数据推入 DMA 缓存。portMAX_DELAY 表示如果缓存满了就死等，直到写完为止
    esp_err_t ret = i2s_channel_write(tx_chan, data, length, &bytes_written, portMAX_DELAY);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写入喇叭数据失败: %s", esp_err_to_name(ret));
    }
}