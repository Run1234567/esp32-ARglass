/**
 * @file speaker_app.c
 * @brief I2S 扬声器输出模块 (带软件音量控制)
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 通过 I2S 标准模式驱动扬声器播放 16kHz/16-bit/单声道 的 PCM 音频数据。
 * 提供软件音量控制功能 (0-100%)，支持静音、无级调节。
 *
 * 硬件连接：
 *   - GPIO 1: WS   (Word Select / LRCK) - 声道选择
 *   - GPIO 2: BCK  (Bit Clock)           - 位时钟
 *   - GPIO 3: DATA (Serial Data)         - 串行音频数据输出
 *
 * 软件音量算法：
 *   对每个 16-bit PCM 采样点进行线性缩放:
 *     output_sample = input_sample * (volume / 100)
 *   为了防止乘法溢出，先提升到 32-bit 进行计算，再裁剪回 16-bit。
 *   100% 音量时跳过计算，直接输出原始数据，节省 CPU。
 *
 * 依赖组件：
 *   - log:    日志输出
 *   - driver: I2S 标准模式驱动 (driver/i2s_std.h)
 */

#include "speaker_app.h"
#include "esp_log.h"
#include "driver/i2s_std.h"  // I2S 标准模式驱动 (Philips/MSB/PCM 格式)

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // 提供 portMAX_DELAY 宏定义

static const char *TAG = "SPEAKER_APP";  // 日志标签

/* =====================================================================
 * 扬声器引脚定义
 * =====================================================================
 * I2S 标准模式需要 3 根信号线:
 *   WS  (Word Select):  声道同步信号，区分左右声道
 *   BCK (Bit Clock):    位时钟，每个时钟传输 1-bit 数据
 *   DATA (Serial Data): 串行音频数据输出
 */
#define SPK_I2S_WS   1   // GPIO 1: 声道选择
#define SPK_I2S_BCK  2   // GPIO 2: 位时钟
#define SPK_I2S_DATA 3   // GPIO 3: 数据输出

/* I2S 发送通道句柄 */
static i2s_chan_handle_t tx_chan;

/* =====================================================================
 * 全局音量控制 (范围: 0 - 100)
 * =====================================================================
 * 默认 100% 音量。通过 set_speaker_volume() 调节。
 * 0 = 静音，100 = 原始音量 (跳过缩放计算)
 */
static uint8_t global_volume = 100;  // 音量百分比 (0-100)
static uint8_t global_gain = 2;     // 软件增益倍数 (1-8)，默认 2 倍放大

/**
 * @brief 设置扬声器音量
 * @param vol 音量值 (0-100)，超出范围自动裁剪到 100
 */
void set_speaker_volume(uint8_t vol) {
    if (vol > 100) vol = 100;  // 防止溢出
    global_volume = vol;
    ESP_LOGI(TAG, "🔊 喇叭音量已调节为: %d%%", global_volume);
}

/**
 * @brief 获取当前音量
 * @return 当前音量值 (0-100)
 */
uint8_t get_speaker_volume(void) {
    return global_volume;
}

/**
 * @brief 设置软件增益倍数
 * @param gain 增益倍数 (1-8)，1=不放大，4=4倍放大
 * 默认 4 倍，TTS/音乐等小信号源建议 4-6 倍
 */
void set_speaker_gain(uint8_t gain) {
    if (gain < 1) gain = 1;
    if (gain > 8) gain = 8;
    global_gain = gain;
    ESP_LOGI(TAG, "🔊 软件增益已设置为: %dx", global_gain);
}

/* =====================================================================
 * 扬声器初始化函数
 * =====================================================================
 * @brief 初始化 I2S 标准模式扬声器输出
 *
 * 初始化步骤：
 *   1. 分配 I2S TX 通道 (自动选择空闲端口，主机模式)
 *   2. 配置 I2S 标准模式参数 (时钟/时隙/引脚)
 *   3. 启动通道
 *
 * 音频参数：
 *   - 采样率: 16000 Hz
 *   - 量化深度: 16-bit
 *   - 声道模式: 单声道 (Mono)
 *   - 数据格式: Philips 标准 (I2S)
 *
 * auto_clear = true: 当缓冲区为空时自动输出静音，
 *   防止播放结束时出现杂音 ("结巴" 现象)
 */
void initSpeaker(void) {
    /* ---- 步骤1: 分配 I2S TX 通道 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // 防结巴关键：缓冲区空时自动输出静音
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));  // 只需要 TX 通道

    /* ---- 步骤2: 配置 I2S 标准模式 ---- */
    i2s_std_config_t std_cfg = {
        // 时钟配置: 16kHz 采样率
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),

        // 时隙配置: Philips 格式, 16-bit, 单声道
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,  // 16-bit 量化
            I2S_SLOT_MODE_MONO         // 单声道
        ),

        // GPIO 引脚映射
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  // 不使用主时钟 (MCLK)
            .bclk = SPK_I2S_BCK,     // 位时钟: GPIO 2
            .ws   = SPK_I2S_WS,      // 声道选择: GPIO 1
            .dout = SPK_I2S_DATA,    // 数据输出: GPIO 3
        },
    };

    /* ---- 步骤3: 应用配置并启动通道 ---- */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
}

/* =====================================================================
 * 带音量控制的播放函数
 * =====================================================================
 * @brief 将 PCM 音频数据通过 I2S 发送给扬声器，支持软件音量调节
 *
 * 算法：
 *   1. 静音 (volume=0): 直接丢弃数据，不输出
 *   2. 满音量 (volume=100): 直接输出原始数据，跳过计算 (性能最优)
 *   3. 其他音量: 申请临时缓冲区，逐采样点缩放后输出
 *
 * 缩放公式: output = input * volume / 100
 * 防爆音保护 (Clipping): 裁剪到 [-32768, 32767] 范围
 *
 * @param data   PCM 音频数据指针 (uint8_t 数组，实际是 int16_t 的字节表示)
 * @param length 数据长度 (字节数)
 */
void playSpeaker(const uint8_t *data, size_t length) {
    /* ---- 静音模式：直接丢弃数据 ---- */
    if (global_volume == 0) return;

    /* ---- 满音量 + 无增益：直接输出，节省 CPU ---- */
    if (global_volume == 100 && global_gain == 1) {
        size_t bytes_written = 0;
        i2s_channel_write(tx_chan, data, length, &bytes_written, pdMS_TO_TICKS(1000));
        return;
    }

    /* ---- 软件音量缩放 ---- */

    // 步骤1: 申请临时内存 (传入的 data 是 const，不能直接修改)
    uint8_t *temp_buf = (uint8_t *)malloc(length);
    if (!temp_buf) {
        ESP_LOGW(TAG, "内存不足，跳过本次音量调节");
        // 内存不足时以原始音量硬播，保证不崩溃
        size_t bw = 0;
        i2s_channel_write(tx_chan, data, length, &bw, pdMS_TO_TICKS(100));
        return;
    }

    // 步骤2: 将字节流视为 16-bit 采样点数组
    int16_t *pcm_in = (int16_t *)data;       // 输入: 原始 PCM 数据
    int16_t *pcm_out = (int16_t *)temp_buf;  // 输出: 缩放后的 PCM 数据
    size_t sample_count = length / 2;         // 采样点数 = 字节数 / 2

    // 步骤3: 遍历每个采样点，进行音量缩放 + 增益放大
    for (size_t i = 0; i < sample_count; i++) {
        // 先提升到 32-bit 做乘法，防止溢出
        int32_t sample = (int32_t)pcm_in[i];

        // 应用软件增益放大 (默认 4 倍)
        sample *= global_gain;

        // 应用音量百分比缩放
        sample = sample * global_volume / 100;

        // 防爆音保护 (Clipping):
        // 16-bit 有符号整数范围: -32768 ~ +32767
        // 超出范围会产生极其刺耳的数字失真杂音
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        pcm_out[i] = (int16_t)sample;
    }

    // 步骤4: 将缩放后的数据写入 I2S 硬件
    size_t bytes_written = 0;
    i2s_channel_write(tx_chan, temp_buf, length, &bytes_written, pdMS_TO_TICKS(1000));

    // 步骤5: 释放临时内存 (必须！否则内存泄漏)
    free(temp_buf);
}
