/**
 * @file main.c
 * @brief J.A.R.V.I.S 智能 AR 眼镜系统 - 主程序入口
 *
 * =====================================================================
 * 系统概述：
 * =====================================================================
 * 这是一个基于 ESP32-S3 的 AR 眼镜固件，代号 "J.A.R.V.I.S"。
 * 系统集成了以下核心功能：
 *   1. 麦克风音频采集与多路分发 (SD卡录音/音调检测/语音唤醒)
 *   2. 中文 TTS 语音合成 (边转边播)
 *   3. 小说/电子书阅读 (SD卡 TXT 文件)
 *   4. "Jarvis" 唤醒词检测 (ESP-SR WakeNet9)
 *   5. YIN 基频检测算法 (音调/音高识别)
 *   6. 分贝计算 (环境噪声监测)
 *   7. 通过 UART 与外部 UI MCU 通信 (1Mbaud)
 *
 * 硬件平台：Seeed XIAO ESP32-S3 (双核 Xtensa LX7, 240MHz, 8MB Flash, OPI PSRAM)
 * 框架：ESP-IDF v5.5.0 + PlatformIO
 *
 * 数据流架构：
 *   PDM麦克风 --> audio_hub_task (Core 1) --+--> sd_ringbuf  --> record_app --> SD卡 WAV录音
 *                                            +--> yin_ringbuf --> yin_pitch_task --> YIN音调检测
 *                                            +--> sr_ringbuf  --> voice_app --> WakeNet9唤醒引擎
 *
 *   小说TXT文件 --> novel_read_task --> tts_app --> playSpeaker() --> I2S扬声器
 *   UI MCU <--UART1@1Mbaud--> my_uart (命令分发中枢)
 *
 * FreeRTOS 任务分配：
 *   Core 0: TTS合成任务、语音唤醒(feed/detect)
 *   Core 1: 音频采集Hub、小说读取、音乐播放、YIN测音
 * =====================================================================
 */

/* ==================== 标准库头文件 ==================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>    // 提供 malloc 和 free
#include <math.h>      // 提供 log2f, sqrtf, log10f, roundf 等数学函数
#include <dirent.h>    // 目录遍历 (scandir 等)

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"       // 任务创建、删除、延时
#include "freertos/semphr.h"     // 信号量 (二值信号量、互斥锁)
#include "freertos/ringbuf.h"    // 环形缓冲区 (用于音频数据多路分发)

/* ==================== ESP-IDF 系统头文件 ==================== */
#include "esp_log.h"             // 日志系统 (ESP_LOGI/ESP_LOGE/ESP_LOGW)
#include "esp_heap_caps.h"       // PSRAM 内存分配 (heap_caps_malloc)
#include "nvs_flash.h"           // 非易失性存储 (NVS)，WiFi 等模块需要
#include "esp_camera.h"          // ESP32 摄像头驱动

/* ==================== ESP-TTS 语音合成头文件 ==================== */
#include "esp_tts.h"                    // ESP-TTS 引擎核心 API
#include "esp_tts_voice_xiaole.h"       // 乐鑫内置的 "小乐" 中文字库定义

/* ==================== 项目自定义组件头文件 ==================== */
#include "wifi_app.h"      // WiFi STA 模式连接模块
#include "camera_app.h"    // 摄像头初始化与拍照模块
#include "audio_app.h"     // PDM I2S 麦克风采集模块
#include "speaker_app.h"   // I2S 扬声器输出模块 (带音量控制)
#include "sd_card_app.h"   // SD 卡挂载、小说阅读、音乐/LRC 扫描模块
#include "my_uart.h"       // UART1 命令接口模块 (与外部 UI MCU 通信)
#include "record_app.h"    // 录音模块 (麦克风 -> SD卡 WAV)
#include "tts_app.h"       // TTS 语音合成模块 (异步队列式)
#include "music_app.h"     // WAV 音乐播放器模块
#include "voice_app.h"     // "Jarvis" 唤醒词检测模块 (ESP-SR WakeNet9)
#include "ai_chat.h"       // AI 语音对话模块 (WebSocket + Opus)

/* ==================== TTS 语音模型外部符号 ==================== */
/**
 * @brief 从 SD 卡加载的 TTS 语音数据起始地址
 *
 * 这个符号对应 CMakeLists.txt 中通过 EMBED_FILES 添加的二进制文件
 * esp_tts_voice_data_xiaole_dat，链接器会自动将其嵌入固件
 * 注意：实际运行时 TTS 模块会优先从 SD 卡加载模型到 PSRAM
 */
extern const uint8_t esp_tts_voice_data_xiaole_dat_start[] asm("_binary_esp_tts_voice_data_xiaole_dat_start");

/* ==================== 全局 TTS 引擎句柄 ==================== */
/**
 * @brief TTS 引擎全局句柄
 * 初始化一次，在整个系统生命周期内复用
 * 用于 esp_tts_parse_chinese() 和 esp_tts_stream_play() 调用
 */
esp_tts_handle_t *tts_handle = NULL;

/* ==================== 日志标签 ==================== */
static const char *TAG = "J.A.R.V.I.S";  // 主程序日志前缀

/* =====================================================================
 * 音频环形缓冲区 (Ring Buffer) - 音频数据多路分发核心
 * =====================================================================
 * 音频 Hub 任务从麦克风读取数据后，需要同时发送给多个消费者：
 *   1. sd_ringbuf  -> SD 卡录音任务 (保存为 WAV 文件)
 *   2. yin_ringbuf -> YIN 音调检测任务 (基频分析)
 *   3. sr_ringbuf  -> 语音唤醒引擎 (WakeNet9 AFE)
 *
 * 使用 RINGBUF_TYPE_NOSPLIT 类型，保证每次写入的数据块
 * 被完整读出，不会被分割到两次读取中
 */
RingbufHandle_t sd_ringbuf = NULL;   // SD 卡录音缓冲区
RingbufHandle_t yin_ringbuf = NULL;  // YIN 音调检测缓冲区
RingbufHandle_t sr_ringbuf = NULL;   // 语音唤醒引擎缓冲区
RingbufHandle_t ai_ringbuf = NULL;   // AI 对话音频缓冲区 (Opus 编码)

/* =====================================================================
 * UI 控制开关 (通过 UART 命令由外部 MCU 控制)
 * ===================================================================== */
volatile bool send_noise_data = false;  // 是否向 UI 发送分贝数据 (CMD:NOISE_ON/OFF)
volatile bool send_pitch_data = false;  // 是否向 UI 发送音调数据 (CMD:PITCH_ON/OFF)

/**
 * @brief 扬声器互斥锁
 *
 * TTS 任务、音乐播放任务 都需要使用扬声器
 * 通过互斥锁保证同一时刻只有一个任务在写 I2S 数据
 * 在 app_main() 中创建，各模块通过 extern 引用
 */
SemaphoreHandle_t speaker_mutex = NULL;

/**
 * @brief 小说翻页信号量 (二值信号量)
 *
 * 工作流程：
 *   1. novel_read_task 在此信号量上阻塞等待
 *   2. 当 TTS 播完当前段落 (tts_app.c) 或 UI 请求下一页 (my_uart.c) 时，
 *      释放信号量
 *   3. novel_read_task 被唤醒，从 SD 卡读取下一段文本
 */
SemaphoreHandle_t next_page_sem = NULL;

/* =====================================================================
 * 函数：calculate_decibel - 分贝计算 (环境噪声监测)
 * =====================================================================
 * @brief 计算音频缓冲区的分贝值 (dBFS + 校准偏移)
 *
 * 算法步骤：
 *   1. 计算所有采样点的均值 (用于去除直流偏置)
 *   2. 计算均方值 (方差): mean_square = E[x²] - (E[x])²
 *   3. 求 RMS (均方根): rms = sqrt(mean_square)
 *   4. 转换为 dBFS: dbfs = 20 * log10(rms / 32768)
 *   5. 加上硬件校准偏移量 (+90dB)，使输出更接近实际声压级
 *
 * @param buffer  16-bit 有符号整数音频缓冲区
 * @param samples 缓冲区中的采样点数量
 * @return 分贝值 (经过校准偏移)，0.0f 表示静音
 */
float calculate_decibel(int16_t *buffer, size_t samples) {
    if (samples == 0 || buffer == NULL) return 0.0f;

    int64_t sum_squares = 0;  // 采样值平方和
    int64_t sum = 0;          // 采样值总和 (用于计算均值)

    for (size_t i = 0; i < samples; i++) {
        int32_t val = buffer[i];
        sum += val;
        sum_squares += (int64_t)val * val;  // 用 int64 防止平方后溢出
    }

    float mean = (float)sum / samples;  // 直流偏置 (理想情况下应为0)

    // 计算方差：E[x²] - (E[x])²
    // 这样可以滤除直流偏置的影响
    float mean_square = (float)sum_squares / samples - (mean * mean);
    if (mean_square <= 0.0f) return 0.0f;  // 方差为负说明数值精度问题

    float rms = sqrtf(mean_square);  // 均方根值
    if (rms < 1.0f) rms = 1.0f;     // 防止 log10(0) 出现 -Inf

    // 转换为 dBFS (满量程分贝)，32768 是 16-bit 音频的最大值
    float dbfs = 20.0f * log10f(rms / 32768.0f);

    // 加上 +90dB 硬件校准偏移量
    // 这个值需要根据实际麦克风灵敏度微调
    return dbfs + 90.0f;
}

/* =====================================================================
 * 频率转音调名称 - 辅助函数
 * =====================================================================
 * 音乐音名对照表 (12平均律):
 *   C  C# D  D# E  F  F# G  G# A  A# B
 *   0  1  2  3  4  5  6  7  8  9  10 11
 */
const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/**
 * @brief 根据频率计算音调名称并打印
 *
 * 使用 MIDI 音符编号公式：
 *   MIDI = 12 * log2(freq / 440) + 69
 * 其中 440Hz = A4 (MIDI 69)
 *
 * 音名 = note_names[MIDI % 12]
 * 八度 = (MIDI / 12) - 1
 * 音分偏差 (cents) = (精确MIDI - 四舍五入MIDI) * 100
 *   正值=偏高，负值=偏低，0=完美
 *
 * @param freq 检测到的基频 (Hz)
 */
void print_pitch_from_freq(float freq) {
    // 过滤太低 (<50Hz，可能是噪声) 或太高 (>2000Hz，超出人声/常规乐器范围) 的杂音
    if (freq <= 50.0f || freq >= 2000.0f) return;

    // 计算 MIDI 音符编号 (浮点，用于计算 cents 偏差)
    float midi_float = 12.0f * log2f(freq / 440.0f) + 69.0f;
    int midi_note = (int)roundf(midi_float);  // 四舍五入到最近的半音

    int note_index = midi_note % 12;         // 音名索引 (0=C, 1=C#, ..., 11=B)
    int octave = (midi_note / 12) - 1;       // 八度编号 (MIDI 60=C4, 69=A4)
    float cents = (midi_float - midi_note) * 100.0f;  // 音分偏差

    ESP_LOGI("PITCH", "🎵 频率: %5.1f Hz -> 音高: %2s%d (偏差: %4.1f cents)",
             freq, note_names[note_index], octave, cents);
}

/* =====================================================================
 * YIN 时域自相关算法 - 高精度基频提取
 * =====================================================================
 * @brief 使用 YIN 算法从音频缓冲区中提取基频 (Fundamental Frequency)
 *
 * YIN 算法原理 (4个步骤):
 *   1. 差分函数 (Difference Function):
 *      对每个候选周期 tau，计算原始信号与 tau 移位信号的平方差之和
 *      d(tau) = Σ(x[i] - x[i+tau])²
 *
 *   2. 累积平均归一化 (Cumulative Mean Normalized Difference):
 *      d'(tau) = d(tau) / ((1/tau) * Σd[j])  for j=1..tau
 *      这一步消除了振幅的影响，使阈值判断更稳定
 *
 *   3. 绝对阈值法 (Absolute Threshold):
 *      从 tau=2 开始，找到第一个 d'(tau) < 阈值(0.15) 的位置
 *      并继续向后搜索直到找到局部最小值
 *
 *   4. 抛物线插值 (Parabolic Interpolation):
 *      在阈值交叉点附近用抛物线拟合，提升精度到亚采样级别
 *      better_tau = tau + (d'[tau+1] - d'[tau-1]) / (2*(2*d'[tau] - d'[tau+1] - d'[tau-1]))
 *
 * 最终频率 = 采样率 / better_tau
 *
 * @param buffer      16-bit 音频缓冲区
 * @param buffer_size 缓冲区大小 (采样点数)
 * @param sample_rate 采样率 (Hz)，本系统固定为 16000
 * @return 检测到的基频 (Hz)，0.0f 表示未检测到有效基频
 */
float calculate_pitch_yin(int16_t *buffer, size_t buffer_size, int sample_rate) {
    int half_size = buffer_size / 2;  // YIN 只需要分析前半段

    // 分配在堆内存，防止撑爆 Task 栈 (half_size * 4 字节)
    float *yin_buffer = (float *)malloc(half_size * sizeof(float));
    if (yin_buffer == NULL) return 0.0f;  // 内存分配失败

    /* ---- 步骤1: 差分函数 ---- */
    // 对每个候选周期 tau (0 ~ half_size-1)，计算信号的自相关差值
    for (int tau = 0; tau < half_size; tau++) {
        yin_buffer[tau] = 0;
        for (int i = 0; i < half_size; i++) {
            float delta = (float)buffer[i] - (float)buffer[i + tau];
            yin_buffer[tau] += delta * delta;  // 累加平方差
        }
    }

    /* ---- 步骤2: 累积平均归一化 ---- */
    yin_buffer[0] = 1;  // tau=0 时差值恒为0，设为1避免除零
    float running_sum = 0;
    for (int tau = 1; tau < half_size; tau++) {
        running_sum += yin_buffer[tau];
        // 累积平均：running_sum / tau 就是前面所有差值的平均
        yin_buffer[tau] *= tau / running_sum;
    }

    /* ---- 步骤3: 绝对阈值法 ---- */
    // 阈值 0.15：较低的阈值意味着更高的检测精度，但可能漏检
    int tau_estimate = -1;
    for (int tau = 2; tau < half_size; tau++) {
        if (yin_buffer[tau] < 0.15f) {
            // 找到第一个低于阈值的点后，继续搜索局部最小值
            while (tau + 1 < half_size && yin_buffer[tau + 1] < yin_buffer[tau]) {
                tau++;
            }
            tau_estimate = tau;
            break;
        }
    }

    // 如果没找到低于阈值的点，说明没有检测到有效基频
    if (tau_estimate == -1) {
        free(yin_buffer);
        return 0.0f;  // 未检测到基频
    }

    /* ---- 步骤4: 抛物线插值 ---- */
    // 在 tau_estimate 附近用三点抛物线插值，提升精度
    float better_tau = tau_estimate;
    if (tau_estimate > 0 && tau_estimate < half_size - 1) {
        float s0 = yin_buffer[tau_estimate - 1];  // 左邻点
        float s1 = yin_buffer[tau_estimate];       // 最低点
        float s2 = yin_buffer[tau_estimate + 1];   // 右邻点
        // 抛物线顶点偏移公式
        better_tau += (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0));
    }

    free(yin_buffer);  // 释放临时内存
    return (float)sample_rate / better_tau;  // 频率 = 采样率 / 周期
}

/* =====================================================================
 * 音频分发中心 (Audio Hub) - 核心采集任务
 * =====================================================================
 * @brief 运行在 Core 1 的音频数据采集与分发任务
 *
 * 职责：
 *   1. 从 PDM 麦克风读取 512 采样点 (1024 字节) 的音频帧
 *   2. 计算分贝值 (如果 UI 开启了噪声监测，通过 UART 发送)
 *   3. 将音频数据分发到 3 个环形缓冲区：
 *      - sd_ringbuf:  SD 卡录音 (仅在录音状态时写入)
 *      - yin_ringbuf: YIN 音调检测 (仅在有声音且开启时写入)
 *      - sr_ringbuf:  语音唤醒引擎 (始终写入，溢出自动丢弃)
 *
 * 优先级: 5 (较高，保证音频实时性)
 * 栈大小: 8192 字节
 * 核心绑定: Core 1 (与 WiFi 隔离，避免 WiFi 中断影响音频采集)
 */
void audio_hub_task(void *pvParameters) {
    const size_t samples = 512;             // 每次读取 512 个采样点
    int16_t audioBuffer[samples];           // 栈上分配音频缓冲区 (1024 字节)

    static TickType_t last_send_time = 0;   // 上次发送分贝数据的时间戳

    ESP_LOGI("AUDIO_HUB", "🎤 麦克风核心采集枢纽已启动");

    while (1) {
        // 从 PDM 麦克风读取 PCM 数据 (阻塞式，超时 10ms)
        size_t bytesRead = readAudio(audioBuffer, samples);

        if (bytesRead > 0) {
            static int hub_dbg = 0;
            if (hub_dbg < 3) {
                ESP_LOGI("HUB", "readAudio OK: %d bytes, loop #%d", bytesRead, hub_dbg);
                hub_dbg++;
            }
            /* ---- 1. 分贝计算 ---- */
            float db_value = calculate_decibel(audioBuffer, samples);

            // 如果 UI 开启了噪声监测，每 100ms 发送一次分贝值
            if (send_noise_data) {
                TickType_t current_time = xTaskGetTickCount();
                if (current_time - last_send_time >= pdMS_TO_TICKS(100)) {
                    char db_cmd[16];
                    snprintf(db_cmd, sizeof(db_cmd), "DB:%d", (int)db_value);
                    my_uart_send(db_cmd);  // 格式: "DB:75"
                    last_send_time = current_time;
                }
            }

            /* ---- 2. 发送给 SD 卡录音 ---- */
            // 仅在录音状态时写入 (is_recording 由 record_app 控制)
            extern volatile bool is_recording;
            if (sd_ringbuf != NULL && is_recording) {
                xRingbufferSend(sd_ringbuf, audioBuffer, bytesRead, 0);
            }

            /* ---- 3. 发送给 YIN 音调检测 ---- */
            // 仅在有声音 (分贝>35) 且 UI 开启了音调检测时写入
            if (yin_ringbuf != NULL && db_value > 35.0f && send_pitch_data) {
                xRingbufferSend(yin_ringbuf, audioBuffer, bytesRead, 0);
            }

            /* ---- 4. 发送给语音唤醒引擎 ---- */
            // 始终写入，超时 0：如果 AFE 处理不过来就自动丢弃，绝不阻塞 Hub
            if (sr_ringbuf != NULL) {
                xRingbufferSend(sr_ringbuf, audioBuffer, bytesRead, 0);
            }

            /* ---- 5. 发送给 AI 对话模块 ---- */
            if (ai_ringbuf != NULL) {
                BaseType_t ret = xRingbufferSend(ai_ringbuf, audioBuffer, bytesRead, 0);
                static int ai_dbg = 0;
                if (ai_dbg < 3) {
                    ESP_LOGI("HUB", "AI写入: ret=%d bytes=%d ringbuf=%p", ret, bytesRead, (void*)ai_ringbuf);
                    ai_dbg++;
                }
            } else {
                static int ai_null_dbg = 0;
                if (ai_null_dbg < 1) {
                    ESP_LOGE("HUB", "ai_ringbuf 是 NULL!");
                    ai_null_dbg++;
                }
            }
        }
    }
}

/* =====================================================================
 * 小说读取任务
 * =====================================================================
 * @brief 独立任务：等待信号量，从 SD 卡读取小说文本并交给 TTS 播报
 *
 * 工作模式：
 *   1. 在 next_page_sem 信号量上阻塞等待 (不消耗 CPU)
 *   2. 收到信号后，调用 test_read_novel_next_chunk() 读取 128 字节
 *   3. 该函数内部会将文本交给 TTS 引擎合成语音
 *   4. TTS 播完后会释放信号量，触发下一段读取
 *
 * 信号量触发源：
 *   - TTS 播完当前队列 (tts_app.c)
 *   - UI 请求下一页 (my_uart.c: CMD:NOVEL_END)
 *   - UI 请求开始阅读新章节 (my_uart.c: CMD:READ_CHAP:)
 *
 * 优先级: 4
 * 栈大小: 8192 字节 (4096*2)
 * 核心绑定: Core 1
 */
void novel_read_task(void *pvParameters) {
    ESP_LOGI("NOVEL_TASK", "📖 小说读取子任务已启动，正在待命...");

    while (1) {
        // 核心：在这里等信号，不干活时完全不占 CPU
        if (xSemaphoreTake(next_page_sem, portMAX_DELAY) == pdTRUE) {

            ESP_LOGI("NOVEL_TASK", "📄 收到翻页信号，开始工作...");

            // 执行具体的读卡和 TTS 播报逻辑
            // 此函数位于 sd_card_app.c 中
            test_read_novel_next_chunk();

            ESP_LOGI("NOVEL_TASK", "✅ 任务完成，继续待命。");
        }
    }

    // 理论上永远不会走到这里，但作为好习惯，任务退出要删除自己
    vTaskDelete(NULL);
}

/* =====================================================================
 * YIN 音调检测任务 (动态启停)
 * =====================================================================
 * @brief 使用 YIN 算法实时检测音频的基频/音调
 *
 * 特点：
 *   - 动态创建/销毁：UI 开启音调检测时创建，关闭时销毁
 *   - 从 yin_ringbuf 累积 2048 个采样点后进行一次 YIN 分析
 *   - 检测到有效频率后，通过 UART 发送给 UI 显示
 *   - 阈值分贝 >35dB 时才会有数据进入 yin_ringbuf (由 Hub 控制)
 *
 * 优先级: 3
 * 栈大小: 8192 字节
 * 核心绑定: Core 1
 */
TaskHandle_t yin_task_handle = NULL;  // 任务句柄，用于判断任务是否在运行

void yin_pitch_task(void *pvParameters);  // 前向声明

/**
 * @brief 启动 YIN 音调检测任务
 * 清空环形缓冲区中的旧数据，然后创建任务
 */
void start_yin_pitch_task(void) {
    if (yin_task_handle == NULL) {
        send_pitch_data = true;
        // 清空环形缓冲区中的残留数据，避免分析过期音频
        size_t dummy;
        void *stale;
        while ((stale = xRingbufferReceive(yin_ringbuf, &dummy, 0)) != NULL) {
            vRingbufferReturnItem(yin_ringbuf, stale);
        }
        // 创建任务，绑定到 Core 1 (栈分配到 PSRAM)
        static StackType_t *yin_stack = NULL;
        static StaticTask_t yin_tcb;
        if (!yin_stack) {
            yin_stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        }
        if (yin_stack) {
            yin_task_handle = xTaskCreateStaticPinnedToCore(yin_pitch_task, "yin_task", 8192,
                NULL, 3, yin_stack, &yin_tcb, 1);
        } else {
            xTaskCreatePinnedToCore(yin_pitch_task, "yin_task", 8192, NULL, 3, &yin_task_handle, 1);
        }
    }
}

/**
 * @brief 停止 YIN 音调检测任务
 * 仅设置标志位，任务会在下次循环时自行退出并释放内存
 */
void stop_yin_pitch_task(void) {
    if (yin_task_handle != NULL) {
        send_pitch_data = false;  // 任务循环条件，设为 false 后任务会退出
    }
}

/**
 * @brief YIN 音调检测任务主体
 *
 * 工作流程：
 *   1. 分配 2048 个采样点的累积缓冲区 (4KB)
 *   2. 从 yin_ringbuf 持续接收音频片段，累积到缓冲区
 *   3. 累积满后调用 calculate_pitch_yin() 计算基频
 *   4. 如果检测到有效频率 (>20Hz)，通过 UART 发送给 UI
 *   5. 清空缓冲区，继续下一轮累积
 *   6. 当 send_pitch_data 设为 false 时，释放内存并退出
 */
void yin_pitch_task(void *pvParameters) {
    const size_t target_samples = 2048;  // YIN 分析需要的采样点数
    int16_t *accum_buffer = malloc(target_samples * sizeof(int16_t));  // 堆上分配 (4KB)
    size_t current_count = 0;   // 当前已累积的采样点数
    size_t item_size;

    ESP_LOGI("YIN_TASK", "🎵 独立测音任务已分配内存并动态启动！");

    // 主循环：持续接收音频数据直到 send_pitch_data 被设为 false
    while (send_pitch_data) {
        // 从环形缓冲区接收音频片段，超时 100ms
        void *audio_data = xRingbufferReceive(yin_ringbuf, &item_size, pdMS_TO_TICKS(100));

        if (audio_data != NULL) {
            size_t samples_received = item_size / sizeof(int16_t);

            // 计算还能容纳多少采样点，防止溢出
            size_t space_left = target_samples - current_count;
            size_t samples_to_copy = (samples_received < space_left) ? samples_received : space_left;

            // 将音频数据追加到累积缓冲区
            memcpy(&accum_buffer[current_count], audio_data, samples_to_copy * sizeof(int16_t));
            current_count += samples_to_copy;

            vRingbufferReturnItem(yin_ringbuf, audio_data);  // 归还内存

            // 累积满一帧，开始分析
            if (current_count >= target_samples) {
                // 使用 YIN 算法计算基频
                float exact_freq = calculate_pitch_yin(accum_buffer, target_samples, 16000);

                if (exact_freq > 20.0f) {
                    // 在日志中打印音调名称
                    print_pitch_from_freq(exact_freq);

                    // 每 100ms 最多发送一次音调数据给 UI (避免刷屏)
                    static TickType_t last_p_time = 0;
                    if (xTaskGetTickCount() - last_p_time > pdMS_TO_TICKS(100)) {
                        char p_cmd[20];
                        snprintf(p_cmd, sizeof(p_cmd), "PH:%.1f", exact_freq);  // 格式: "PH:440.0"
                        my_uart_send(p_cmd);
                        last_p_time = xTaskGetTickCount();
                    }
                }
                current_count = 0;  // 清空累积缓冲区，开始下一轮
                vTaskDelay(pdMS_TO_TICKS(10));  // 短暂让出 CPU
            }
        }
    }

    /* ---- 任务退出清理 ---- */
    free(accum_buffer);     // 释放 4KB 堆内存
    yin_task_handle = NULL; // 清空句柄，允许下次重新创建
    ESP_LOGI("YIN_TASK", "🔇 界面已关闭，测音任务安全退出，已归还 8KB 内存！");
    vTaskDelete(NULL);      // 删除自身任务
}

/* =====================================================================
 * app_main - 系统入口函数
 * =====================================================================
 * @brief ESP-IDF 应用程序入口点，按顺序完成所有模块的初始化
 *
 * 初始化顺序：
 *   1. 延时 3s (等待电源稳定)
 *   2. NVS 初始化 (WiFi 等模块依赖)
 *   3. 创建扬声器互斥锁
 *   4. SD 卡初始化与读写测试
 *   5. 创建 3 个音频环形缓冲区
 *   6. 初始化摄像头、麦克风、扬声器
 *   7. 初始化 TTS 引擎 (从 SD 卡加载模型)
 *   8. 启动 Jarvis 唤醒引擎
 *   9. 创建小说翻页信号量
 *  12. 初始化 UART 命令接口
 *  13. 创建 2 个 FreeRTOS 任务 (音频Hub/小说读取)
 *  15. TTS 播报启动欢迎语
 *  16. 主线程进入空循环 (所有工作由子任务完成)
 */
void app_main(void) {
    // 延时 3 秒，等待电源稳定和外设上电完成
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ---- 步骤1: 初始化 NVS (非易失性存储) ---- */
    // NVS 用于存储 WiFi 配置等数据，是 esp_wifi 的前置依赖
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      // NVS 分区版本不匹配或空间不足，擦除后重新初始化
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- 步骤2: 创建扬声器互斥锁 ---- */
    // TTS、音乐播放 都需要使用扬声器
    // 互斥锁保证同一时刻只有一个任务在写 I2S 数据
    speaker_mutex = xSemaphoreCreateMutex();

    if (speaker_mutex == NULL) {
        ESP_LOGE("MAIN", "❌ 致命错误：喇叭互斥锁创建失败！");
        return;  // 如果锁没造出来，后面的系统就别跑了
    }

    /* ---- 步骤3: 初始化 SD 卡 ---- */

    if (init_sd_card() == ESP_OK) {
        test_sd_card_read_write();  // 简单读写测试，验证 SD 卡工作正常
    } else {
        ESP_LOGE(TAG, "💾 SD 卡模块异常，跳过后续依赖任务...");
    }

    /* ---- 步骤4: 创建音频环形缓冲区 ---- */
    // 每个缓冲区分配 10KB，足以缓冲数个音频帧 (512 samples * 2 bytes = 1024 bytes/帧)
    // RINGBUF_TYPE_NOSPLIT: 保证每次写入的数据被完整读出
    sd_ringbuf  = xRingbufferCreate(10240, RINGBUF_TYPE_NOSPLIT);  // SD卡录音
    yin_ringbuf = xRingbufferCreate(8192,  RINGBUF_TYPE_NOSPLIT);  // YIN测音
    sr_ringbuf  = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);  // 语音唤醒 (稍大，避免丢帧)
    ai_ringbuf  = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);  // AI 对话 (Opus 编码)

    if (sd_ringbuf == NULL) {
        ESP_LOGE(TAG, "❌ 致命错误：音频环形缓冲区创建失败！");
        return;
    }

    /* ---- 步骤5: 初始化硬件外设 ---- */
    initCamera();   // OV2640 并行摄像头 (JPEG/UXGA 模式)
    initAudio();    // PDM I2S 麦克风 (GPIO 41/42, 16kHz/16bit/单声道)
    initSpeaker();  // I2S 扬声器 (GPIO 1/2/3, 16kHz/16bit/单声道)

    /* ---- 步骤6: 初始化软件引擎 ---- */
    init_tts_engine();      // TTS 语音合成引擎 (从 SD 卡加载模型到 PSRAM)
    start_jarvis_brain();   // "Jarvis" 唤醒词检测引擎 (WakeNet9 + AFE)
    wifi_init_sta();        // WiFi STA 连接 (AI 对话依赖网络)
    ai_chat_init();         // AI 语音对话模块 (WebSocket + Opus)

    /* ---- 步骤7: 创建小说翻页信号量 ---- */
    next_page_sem = xSemaphoreCreateBinary();
    if (next_page_sem == NULL) {
        ESP_LOGE(TAG, "致命错误：信号量创建失败，内存不足！");
        return;
    }

    /* ---- 步骤8: 初始化 UART 命令接口 ---- */
    // UART1 @ 1Mbaud，GPIO 4(TX)/5(RX)，用于与外部 UI MCU 通信
    my_uart_init();

    /* ---- 步骤9: 创建 FreeRTOS 任务 ---- */
    // 1. 麦克风核心采集任务 (音频 Hub)
    xTaskCreatePinnedToCore(audio_hub_task, "audio_hub", 8192, NULL, 5, NULL, 1);

    // 2. 小说读取任务 (栈分配到 PSRAM)
    {
        static StackType_t *novel_stack = NULL;
        static StaticTask_t novel_tcb;
        if (!novel_stack) {
            novel_stack = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
        }
        if (novel_stack) {
            xTaskCreateStaticPinnedToCore(novel_read_task, "novel_task", 4096 * 2,
                NULL, 4, novel_stack, &novel_tcb, 1);
            ESP_LOGI(TAG, "小说任务栈已分配到 PSRAM (8KB)");
        } else {
            xTaskCreatePinnedToCore(novel_read_task, "novel_task", 4096 * 2, NULL, 4, NULL, 1);
        }
    }

    /* ---- 步骤10: TTS 播报启动欢迎语 ---- */
    tts_speak("贾维斯系统已启动。主脑连接成功，正在等待指令。");

    /* ---- 主线程进入空循环 ---- */
    // 所有实际工作都由上面创建的子任务完成
    // 主线程只需要保持存活，不要退出 app_main()
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // 每 10 秒唤醒一次 (实际上不需要)
    }
}
