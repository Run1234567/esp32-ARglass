/**
 * @file audio_app.c
 * @brief PDM I2S 麦克风音频采集模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 初始化并管理 PDM (Pulse Density Modulation) 数字麦克风。
 * 通过 I2S 接口以 16kHz 采样率、16-bit 量化深度、单声道模式采集音频数据。
 *
 * 硬件连接：
 *   - GPIO 42: PDM 时钟输出 (CLK) - 驱动麦克风的采样时钟
 *   - GPIO 41: PDM 数据输入 (DIN) - 接收麦克风的 PDM 数据流
 *
 * 技术说明：
 *   PDM 是一种 1-bit 高采样率的调制方式，与传统的 PCM 不同。
 *   ESP32-S3 的 I2S 硬件内置了 PDM 解码器，会自动将 PDM 信号
 *   转换为 16-bit PCM 数据，软件层面读取的就是标准 PCM 采样。
 *
 * 依赖组件：
 *   - log:     日志输出
 *   - driver:  I2S 驱动 (driver/i2s_pdm.h)
 */

#include "audio_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== ESP-IDF I2S PDM 驱动 ==================== */
#include "driver/i2s_pdm.h"  // ESP-IDF v5 专用的 PDM I2S 驱动

static const char *TAG = "AUDIO_APP";  // 日志标签

/* =====================================================================
 * 麦克风引脚定义
 * =====================================================================
 * 根据 PCB 布局确定，修改时需同步更新硬件
 */
#define I2S_WS_IO   42  // PDM 时钟输出引脚 (Word Select / Clock)
#define I2S_SD_IO   41  // PDM 数据输入引脚 (Serial Data)

/* =====================================================================
 * I2S 接收通道句柄
 * =====================================================================
 * 保存 I2S 通道实例，后续 readAudio() 调用需要使用此句柄
 */
static i2s_chan_handle_t rx_chan;

/* =====================================================================
 * 麦克风初始化函数
 * =====================================================================
 * @brief 初始化 PDM I2S 麦克风，配置采样参数并启动通道
 *
 * 初始化步骤：
 *   1. 分配 I2S 通道 (自动选择空闲的 I2S 端口，设为主机模式)
 *   2. 配置 PDM RX 模式参数 (采样率/量化深度/引脚映射)
 *   3. 将配置应用到通道
 *   4. 启动通道，开始接收数据
 *
 * 采样参数：
 *   - 采样率: 16000 Hz (16kHz，适合语音处理)
 *   - 量化深度: 16-bit (int16_t)
 *   - 声道模式: 单声道 (Mono)
 */
void initAudio(void) {
    ESP_LOGI(TAG, "Initializing PDM Microphone...");

    /* ---- 步骤1: 分配 I2S 通道 ---- */
    // I2S_NUM_AUTO: 自动选择空闲的 I2S 端口 (ESP32-S3 有 I2S0 和 I2S1)
    // I2S_ROLE_MASTER: ESP32 作为主机，提供时钟信号给麦克风
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));  // 只需要 RX 通道

    /* ---- 步骤2: 配置 PDM RX 模式参数 ---- */
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        // 时钟配置: 采样率 16000Hz
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),

        // 时隙配置: 16-bit 采样深度，单声道模式
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,  // 16-bit 量化
            I2S_SLOT_MODE_MONO         // 单声道
        ),

        // GPIO 引脚映射
        .gpio_cfg = {
            .clk = I2S_WS_IO,   // 时钟引脚 (GPIO 42)
            .din = I2S_SD_IO,   // 数据引脚 (GPIO 41)
            .invert_flags = {
                .clk_inv = false,  // 时钟不反转
            },
        },
    };

    /* ---- 步骤3: 应用配置并启动通道 ---- */
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "🎤 麦克风 I2S PDM 模式初始化成功！");
}

/* =====================================================================
 * 音频数据读取函数
 * =====================================================================
 * @brief 从 PDM 麦克风读取一帧 PCM 音频数据
 *
 * @param buffer  目标缓冲区指针 (int16_t 数组)
 * @param samples 要读取的采样点数量
 * @return 实际读取的字节数 (0 表示超时或出错)
 *
 * 注意：
 *   - 此函数是阻塞式的，最长等待 10ms
 *   - 每个采样点占 2 字节 (16-bit)
 *   - 在高频读取时建议注释掉错误日志，避免刷屏
 */
size_t readAudio(int16_t* buffer, size_t samples) {
    size_t bytes_read = 0;

    // 从 I2S 通道读取数据
    // 参数: 通道句柄, 目标缓冲区, 要读取的字节数, 实际读取字节数, 超时时间
    esp_err_t ret = i2s_channel_read(
        rx_chan,
        buffer,
        samples * sizeof(int16_t),  // 要读取的字节数 = 采样点数 * 2
        &bytes_read,
        pdMS_TO_TICKS(10)           // 超时 10ms，防止永久阻塞
    );

    if (ret != ESP_OK) {
        // 超时或出错 (高频读取时建议注释掉这行日志防刷屏)
        // ESP_LOGW(TAG, "Audio read timeout or error");
    }

    return bytes_read;  // 返回实际读取的字节数
}
