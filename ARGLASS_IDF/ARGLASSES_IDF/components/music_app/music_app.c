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

typedef enum { MUSIC_STOPPED, MUSIC_PLAYING, MUSIC_PAUSED } music_state_t;
volatile music_state_t music_state = MUSIC_STOPPED;
volatile int seek_target_sec = -1;
volatile int current_vol = 100;

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

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    int total_sec = (file_size - 44) / 32000;
    
    char cmd_buf[32];
    snprintf(cmd_buf, sizeof(cmd_buf), "AUDIO_INFO:TOT:%d", total_sec);
    my_uart_send(cmd_buf);
    vTaskDelay(pdMS_TO_TICKS(50));

    fseek(f, 44, SEEK_SET);
    uint8_t *buffer = (uint8_t *)malloc(CHUNK_SIZE);
    
    music_state = MUSIC_PLAYING;

    uint32_t samples_played = 0;
    int last_sent_sec = -1;

    while (music_state != MUSIC_STOPPED) {
        if (music_state == MUSIC_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (seek_target_sec >= 0) {
            samples_played = seek_target_sec * 16000;
            long target_offset = 44 + (seek_target_sec * 32000);
            if (target_offset > file_size) target_offset = file_size - CHUNK_SIZE;
            if (target_offset < 44) target_offset = 44;
            
            fseek(f, target_offset, SEEK_SET);
            seek_target_sec = -1;
            ESP_LOGI(TAG, "⏩ 进度已跳转");
        }

        size_t bytes_read = fread(buffer, 1, CHUNK_SIZE, f);
        if (bytes_read > 0) {
            if (current_vol < 100) {
                int16_t *pcm = (int16_t *)buffer;
                for (int i = 0; i < bytes_read / 2; i++) {
                    pcm[i] = (pcm[i] * current_vol) / 100;
                }
            }

            xSemaphoreTake(speaker_mutex, portMAX_DELAY);
            playSpeaker(buffer, bytes_read);
            xSemaphoreGive(speaker_mutex);

            samples_played += bytes_read / 2;
            int cur_sec = samples_played / 16000;
            if (cur_sec != last_sent_sec) {
                char time_cmd[32];
                snprintf(time_cmd, sizeof(time_cmd), "AUDIO_INFO:CUR:%d", cur_sec);
                my_uart_send(time_cmd);
                last_sent_sec = cur_sec;
            }

            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            break;
        }
    }
    
    free(buffer);
    fclose(f);
    free(file_path);
    music_state = MUSIC_STOPPED;
    vTaskDelete(NULL);
}

void start_music_player(const char *path) {
    if (music_state != MUSIC_STOPPED) music_state = MUSIC_STOPPED;
    vTaskDelay(pdMS_TO_TICKS(100));
    char *path_copy = strdup(path);
    xTaskCreatePinnedToCore(play_wav_task, "wav_player", 4096, (void *)path_copy, 4, NULL, 1);
}
void pause_music_player(void) { if (music_state == MUSIC_PLAYING) music_state = MUSIC_PAUSED; }
void resume_music_player(void) { if (music_state == MUSIC_PAUSED) music_state = MUSIC_PLAYING; }
void stop_music_player(void) { music_state = MUSIC_STOPPED; }
void seek_music_player(int sec) { seek_target_sec = sec; }
void set_music_volume(int vol) { current_vol = vol; }
