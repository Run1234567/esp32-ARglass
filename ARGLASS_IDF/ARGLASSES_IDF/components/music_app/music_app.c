#include "music_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "speaker_app.h"
#include "my_uart.h"

static const char *TAG = "MUSIC_APP";
#define CHUNK_SIZE 2048
extern SemaphoreHandle_t speaker_mutex;

// 播放器状态机
typedef enum { MUSIC_STOPPED, MUSIC_PLAYING, MUSIC_PAUSED } music_state_t;
volatile music_state_t music_state = MUSIC_STOPPED;
volatile int seek_target_sec = -1; // -1表示不跳转，大于等于0表示要跳转的秒数

static void play_wav_task(void *pvParameters) {
    char *file_path = (char *)pvParameters;
    
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 找不到文件: %s", file_path);
        free(file_path);
        music_state = MUSIC_STOPPED;
        vTaskDelete(NULL);
        return;
    }

    // 计算音频总时长
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    int total_sec = (file_size - 44) / 32000; // 16kHz 16bit mono = 32000 bytes/sec
    
    // 告诉 UI 界面这首歌有多长
    char cmd_buf[64];
    snprintf(cmd_buf, sizeof(cmd_buf), "AUDIO_INFO:TOTAL:%d\r\n", total_sec);
    my_uart_send(cmd_buf);

    fseek(f, 44, SEEK_SET); // 回到音频数据区开头
    uint8_t *buffer = (uint8_t *)malloc(CHUNK_SIZE);
    
    music_state = MUSIC_PLAYING;

    while (music_state != MUSIC_STOPPED) {
        // 1. 处理暂停状态
        if (music_state == MUSIC_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 2. 处理快进/快退 (Seek)
        if (seek_target_sec >= 0) {
            long target_offset = 44 + (seek_target_sec * 32000);
            if (target_offset > file_size) target_offset = file_size - CHUNK_SIZE;
            if (target_offset < 44) target_offset = 44;
            
            fseek(f, target_offset, SEEK_SET);
            seek_target_sec = -1; // 执行完毕，重置标志
            ESP_LOGI(TAG, "⏩ 进度已跳转");
        }

        // 3. 正常读取与播放
        size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, f);
        if (bytes_read > 0) {
            xSemaphoreTake(speaker_mutex, portMAX_DELAY);
            playSpeaker(buffer, bytes_read);
            xSemaphoreGive(speaker_mutex);
            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            break; // 播完了
        }
    }
    
    free(buffer);
    fclose(f);
    free(file_path);
    music_state = MUSIC_STOPPED;
    vTaskDelete(NULL);
}

// ============ 对外接口 ============
void start_music_player(const char *path) {
    if (music_state != MUSIC_STOPPED) music_state = MUSIC_STOPPED;
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待上一个任务销毁
    char *path_copy = strdup(path);
    xTaskCreatePinnedToCore(play_wav_task, "wav_player", 4096, (void *)path_copy, 4, NULL, 1);
}
void pause_music_player(void) { if (music_state == MUSIC_PLAYING) music_state = MUSIC_PAUSED; }
void resume_music_player(void) { if (music_state == MUSIC_PAUSED) music_state = MUSIC_PLAYING; }
void stop_music_player(void) { music_state = MUSIC_STOPPED; }
void seek_music_player(int sec) { seek_target_sec = sec; }
