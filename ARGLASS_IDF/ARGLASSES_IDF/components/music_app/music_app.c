#include "music_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

// ✨ 只需要引入喇叭驱动（里面已经有了 extern SemaphoreHandle_t speaker_mutex; 声明）
#include "speaker_app.h" 
// ❌ 删除了 tts_app.h，因为我们不再需要 tts_is_speaking 标志位了

static const char *TAG = "MUSIC_APP";
#define CHUNK_SIZE 2048 // 每次读取 2KB 数据
extern SemaphoreHandle_t speaker_mutex;
// 独立的音乐播放任务
static void play_wav_task(void *pvParameters) {
    // 提取传入的文件路径
    char *file_path = (char *)pvParameters;
    
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 找不到音乐文件: %s", file_path);
        free(file_path);
        vTaskDelete(NULL);
        return;
    }

    // 跳过 WAV 文件的 44 字节头部，直奔音频 PCM 数据区
    fseek(f, 44, SEEK_SET);

    uint8_t *buffer = (uint8_t *)malloc(CHUNK_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "❌ 内存不足，无法播放音乐！");
        fclose(f);
        free(file_path);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "🎵 开始播放音乐: %s", file_path);
    size_t bytes_read;

    // 循环读取数据并推给喇叭
    while ((bytes_read = fread(buffer, 1, CHUNK_SIZE, f)) > 0) {
        
        // ✨✨ 1. 抢夺喇叭使用权！
        // 如果贾维斯正在说话，音乐任务会在这里瞬间被挂起（不占CPU），直到贾维斯说完。
        xSemaphoreTake(speaker_mutex, portMAX_DELAY);

        // 此时喇叭没人用，往喇叭里送音乐数据
        playSpeaker(buffer, bytes_read);

        // ✨✨ 2. 播完这一小块，立刻释放喇叭使用权！
        // 这一瞬间，如果高优先级的贾维斯在排队，钥匙就会立刻被贾维斯抢走。
        xSemaphoreGive(speaker_mutex);
        
        // 稍微释放一下 CPU，保证看门狗和其他任务存活
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGI(TAG, "✅ 音乐播放完毕");
    
    free(buffer);
    fclose(f);
    free(file_path); // 释放路径内存
    vTaskDelete(NULL); // 任务使命完成，自动销毁
}

// ==========================================
// 外部调用接口：启动音乐播放
// ==========================================
void start_music_player(const char *path) {
    if (path == NULL) return;

    // 拷贝一份路径字符串，防止外部局部变量被销毁导致野指针
    char *path_copy = strdup(path);
    if (path_copy == NULL) return;

    // 启动一个独立的后台任务来放音乐
    // 优先级设为 4，必须低于 TTS 任务的优先级 (例如 5)
    xTaskCreatePinnedToCore(play_wav_task, "wav_player", 4096, (void *)path_copy, 4, NULL, 1);
}