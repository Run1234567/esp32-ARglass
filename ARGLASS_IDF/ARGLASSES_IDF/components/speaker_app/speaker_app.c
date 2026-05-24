#include "speaker_app.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

// 必须加上下面这两行，编译器才能识别 portMAX_DELAY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SPEAKER_APP";

// 引脚定义
#define SPK_I2S_WS   1
#define SPK_I2S_BCK  2
#define SPK_I2S_DATA 3

static i2s_chan_handle_t tx_chan;

// ==========================================
// ✨ 新增：全局音量控制 (范围: 0 - 100)
// ==========================================
static uint8_t global_volume = 100; // 默认 100% 音量

// 对外接口：设置音量
void set_speaker_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    global_volume = vol;
    ESP_LOGI(TAG, "🔊 喇叭音量已调节为: %d%%", global_volume);
}

// 对外接口：获取当前音量
uint8_t get_speaker_volume(void) {
    return global_volume;
}
// ==========================================

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

// 🎵 升级版：带音量控制的播放函数
void playSpeaker(const uint8_t *data, size_t length) {
    if (global_volume == 0) return; // 静音状态直接丢弃数据

    // 如果是 100% 音量，原封不动直接输出，节省 CPU 算力
    if (global_volume == 100) {
        size_t bytes_written = 0;
        i2s_channel_write(tx_chan, data, length, &bytes_written, pdMS_TO_TICKS(1000));
        return;
    }

    // --- ✨ 核心：软件音量缩放算法 ---
    
    // 1. 申请一块临时内存来装处理后的数据（因为传进来的 data 是 const 的，不能直接改）
    uint8_t *temp_buf = (uint8_t *)malloc(length);
    if (!temp_buf) {
        ESP_LOGW(TAG, "内存不足，跳过本次音量调节");
        // 内存不够就原音量硬播，保证不崩溃
        size_t bw = 0;
        i2s_channel_write(tx_chan, data, length, &bw, pdMS_TO_TICKS(100));
        return;
    }

    // 2. 因为我们是 16bit 音频，把字节流当作 16位 的 int16_t 数组来处理
    int16_t *pcm_in = (int16_t *)data;
    int16_t *pcm_out = (int16_t *)temp_buf;
    size_t sample_count = length / 2; // 两个字节拼成一个音频采样点

    // 3. 遍历每一个音频采样点，进行按比例缩放
    for (size_t i = 0; i < sample_count; i++) {
        // 先升成 32 位整数做乘法，防止计算时溢出
        int32_t sample = (int32_t)pcm_in[i] * global_volume / 100;
        
        // 🛡️ 硬件防爆音保护 (Clipping)
        // 16位有符号整数的极限是 -32768 到 32767，超出这个范围喇叭就会发出极其刺耳的杂音
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        pcm_out[i] = (int16_t)sample;
    }

    // 4. 把处理好的“变弱”的波形发给 I2S 硬件
    size_t bytes_written = 0;
    i2s_channel_write(tx_chan, temp_buf, length, &bytes_written, pdMS_TO_TICKS(1000));
    
    // 5. 务必释放内存
    free(temp_buf);
}