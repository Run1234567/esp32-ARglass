/**
 * @file music_app.c
 * @brief WAV 音乐播放器模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 从 SD 卡读取 WAV 格式音频文件并通过扬声器播放。
 * 支持播放/暂停/停止/跳转/音量控制等完整播放器功能。
 *
 * 音频格式要求：
 *   - 格式: PCM WAV (无压缩)
 *   - 采样率: 16000 Hz
 *   - 量化深度: 16-bit
 *   - 声道: 单声道
 *   - 文件头: 标准 44 字节 WAV 头
 *
 * 播放状态机：
 *   MUSIC_STOPPED -> MUSIC_PLAYING -> MUSIC_PAUSED
 *                   MUSIC_PLAYING -> MUSIC_STOPPED
 *                   MUSIC_PAUSED  -> MUSIC_PLAYING
 *                   MUSIC_PAUSED  -> MUSIC_STOPPED
 *
 * 数据流：
 *   SD卡 WAV文件 -> fread(2KB) -> 音量缩放 -> playSpeaker() -> I2S扬声器
 *
 * UI 通信：
 *   - AUDIO_INFO:TOT:秒 - 总时长 (播放开始时发送)
 *   - AUDIO_INFO:CUR:秒 - 当前进度 (每秒更新)
 *
 * 依赖组件：
 *   - speaker_app: 扬声器播放 (playSpeaker, speaker_mutex)
 *   - tts_app:     speaker_mutex 互斥锁
 *   - my_uart:     串口通信 (上报播放进度)
 */

#include "music_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include "speaker_app.h"  // 扬声器播放
#include "my_uart.h"      // 串口通信

static const char *TAG = "MUSIC_APP";  // 日志标签

/* =====================================================================
 * 配置常量
 * ===================================================================== */
#define CHUNK_SIZE 2048  // 每次从 SD 卡读取的字节数 (2KB)

/**
 * @brief 扬声器互斥锁 (在 main.c 中创建，TTS 模块声明)
 * 音乐播放和 TTS 共享同一个扬声器，需要互斥访问
 */
extern SemaphoreHandle_t speaker_mutex;

/* =====================================================================
 * 播放状态机
 * ===================================================================== */
typedef enum {
    MUSIC_STOPPED,  // 停止状态 (空闲)
    MUSIC_PLAYING,  // 正在播放
    MUSIC_PAUSED    // 暂停中
} music_state_t;

volatile music_state_t music_state = MUSIC_STOPPED;  // 当前播放状态
volatile int seek_target_sec = -1;   // 跳转目标 (秒)，-1 表示不跳转
volatile int current_vol = 100;      // 当前音量 (0-100)

/* =====================================================================
 * WAV 播放任务
 * =====================================================================
 * @brief 独立任务：从 SD 卡读取 WAV 文件并播放
 *
 * 工作流程：
 *   1. 打开 WAV 文件，计算总时长
 *   2. 跳过 44 字节的 WAV 文件头
 *   3. 循环读取 2KB 数据块
 *   4. 应用软件音量缩放
 *   5. 通过扬声器播放 (需获取 speaker_mutex)
 *   6. 每秒上报播放进度给 UI
 *   7. 支持暂停/恢复/跳转/停止操作
 *
 * 优先级: 4
 * 栈大小: 4096 字节
 * 核心绑定: Core 1
 */
static void play_wav_task(void *pvParameters) {
    char *file_path = (char *)pvParameters;  // 文件路径 (由 strdup 分配)

    /* ---- 打开 WAV 文件 ---- */
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 找不到文件: %s", file_path);
        free(file_path);
        music_state = MUSIC_STOPPED;
        vTaskDelete(NULL);
        return;
    }

    /* ---- 计算总时长 ---- */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    // 总时长 = (文件大小 - 44字节WAV头) / (16000采样率 * 2字节)
    int total_sec = (file_size - 44) / 32000;

    // 通过 UART 告诉 UI 总时长
    char cmd_buf[32];
    snprintf(cmd_buf, sizeof(cmd_buf), "AUDIO_INFO:TOT:%d", total_sec);
    my_uart_send(cmd_buf);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 跳过 WAV 文件头，定位到音频数据 ---- */
    fseek(f, 44, SEEK_SET);  // 标准 PCM WAV 头固定 44 字节

    /* ---- 分配读取缓冲区 ---- */
    uint8_t *buffer = (uint8_t *)malloc(CHUNK_SIZE);

    music_state = MUSIC_PLAYING;

    uint32_t samples_played = 0;  // 已播放的采样点数 (用于计算进度)
    int last_sent_sec = -1;        // 上次上报进度的秒数 (避免重复发送)

    /* ---- 主播放循环 ---- */
    while (music_state != MUSIC_STOPPED) {

        /* ---- 暂停处理 ---- */
        if (music_state == MUSIC_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 暂停时休眠等待
            continue;
        }

        /* ---- 跳转处理 ---- */
        if (seek_target_sec >= 0) {
            // 计算目标采样点和文件偏移
            samples_played = seek_target_sec * 16000;  // 16kHz 采样率
            long target_offset = 44 + (seek_target_sec * 32000);  // 16-bit 单声道 = 32000 字节/秒

            // 边界检查
            if (target_offset > file_size) target_offset = file_size - CHUNK_SIZE;
            if (target_offset < 44) target_offset = 44;

            fseek(f, target_offset, SEEK_SET);
            seek_target_sec = -1;  // 重置跳转请求
            ESP_LOGI(TAG, "⏩ 进度已跳转");
        }

        /* ---- 读取音频数据 ---- */
        size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, f);
        if (bytes_read > 0) {

            /* ---- 软件音量缩放 ---- */
            if (current_vol < 100) {
                int16_t *pcm = (int16_t *)buffer;
                for (int i = 0; i < bytes_read / 2; i++) {
                    pcm[i] = (pcm[i] * current_vol) / 100;
                }
            }

            /* ---- 通过扬声器播放 (需获取互斥锁) ---- */
            xSemaphoreTake(speaker_mutex, portMAX_DELAY);
            playSpeaker(buffer, bytes_read);
            xSemaphoreGive(speaker_mutex);

            /* ---- 更新播放进度 ---- */
            samples_played += bytes_read / 2;  // 字节数转采样点数
            int cur_sec = samples_played / 16000;

            // 每秒上报一次进度 (避免 UART 拥塞)
            if (cur_sec != last_sent_sec) {
                char time_cmd[32];
                snprintf(time_cmd, sizeof(time_cmd), "AUDIO_INFO:CUR:%d", cur_sec);
                my_uart_send(time_cmd);
                last_sent_sec = cur_sec;
            }

            vTaskDelay(pdMS_TO_TICKS(2));  // 短暂让出 CPU
        } else {
            break;  // 文件读取完毕
        }
    }

    /* ---- 清理资源 ---- */
    free(buffer);
    fclose(f);
    free(file_path);        // 释放 strdup 分配的路径
    music_state = MUSIC_STOPPED;
    vTaskDelete(NULL);       // 删除自身任务
}

/* =====================================================================
 * 对外接口：开始播放
 * =====================================================================
 * @brief 停止当前播放，创建新任务播放指定 WAV 文件
 *
 * @param path WAV 文件完整路径 (如 "/sdcard/音乐/歌曲1.wav")
 */
void start_music_player(const char *path) {
    // 如果正在播放，先停止
    if (music_state != MUSIC_STOPPED) music_state = MUSIC_STOPPED;
    vTaskDelay(pdMS_TO_TICKS(100));  // 等待旧任务退出

    // 复制路径 (任务参数需要独立内存)
    char *path_copy = strdup(path);

    // 创建播放任务，绑定到 Core 1 (栈分配到 PSRAM)
    static StackType_t *wav_stack = NULL;
    static StaticTask_t wav_tcb;
    if (!wav_stack) {
        wav_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    }
    if (wav_stack) {
        xTaskCreateStaticPinnedToCore(play_wav_task, "wav_player", 4096,
            (void *)path_copy, 4, wav_stack, &wav_tcb, 1);
    } else {
        xTaskCreatePinnedToCore(play_wav_task, "wav_player", 4096, (void *)path_copy, 4, NULL, 1);
    }
}

/* =====================================================================
 * 对外接口：暂停/恢复/停止/跳转/音量
 * ===================================================================== */

/** @brief 暂停播放 */
void pause_music_player(void) {
    if (music_state == MUSIC_PLAYING) music_state = MUSIC_PAUSED;
}

/** @brief 恢复播放 */
void resume_music_player(void) {
    if (music_state == MUSIC_PAUSED) music_state = MUSIC_PLAYING;
}

/** @brief 停止播放 */
void stop_music_player(void) {
    music_state = MUSIC_STOPPED;
}

/** @brief 跳转到指定秒数 */
void seek_music_player(int sec) {
    seek_target_sec = sec;
}

/** @brief 设置播放音量 (0-100) */
void set_music_volume(int vol) {
    current_vol = vol;
}
