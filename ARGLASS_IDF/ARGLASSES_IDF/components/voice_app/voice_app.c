#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

// 乐鑫 ESP-SR 核心库 (只保留唤醒和降噪相关)
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

// 注意：已经彻底删除了所有 esp_mn_xxx (MultiNet) 的头文件

static const char *TAG = "VOICE_APP";

extern RingbufHandle_t sr_ringbuf;
extern srmodel_list_t *esp_srmodel_init(const char *partition_label);

// ==========================================
// 全局句柄
// ==========================================
static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
// 彻底删除了 MultiNet 的句柄和状态变量

// ==========================================
// 数据搬运工：从 Ringbuf 捞水，放大后喂给 AFE
// (保留了你之前的 x4 软件放大器，这对麦克风极其重要！)
// ==========================================
static void feed_audio_task(void *arg) {
    int chunk_size = afe_handle->get_feed_chunksize(afe_data); 
    size_t chunk_bytes = chunk_size * sizeof(int16_t);
    
    int16_t *feed_buffer = malloc(chunk_bytes);
    size_t current_bytes_in_buffer = 0;
    size_t item_size = 0;

    ESP_LOGI(TAG, "🎙️ 纯净版唤醒搬运工已就绪...");

    while (1) {
        void *audio_data = xRingbufferReceive(sr_ringbuf, &item_size, portMAX_DELAY);
        if (audio_data != NULL) {
            uint8_t *src_ptr = (uint8_t *)audio_data;
            size_t bytes_left_to_process = item_size;

            while (bytes_left_to_process > 0) {
                size_t space_left = chunk_bytes - current_bytes_in_buffer;
                size_t copy_size = (bytes_left_to_process < space_left) ? bytes_left_to_process : space_left;

                memcpy((uint8_t *)feed_buffer + current_bytes_in_buffer, src_ptr, copy_size);
                
                current_bytes_in_buffer += copy_size;
                src_ptr += copy_size;
                bytes_left_to_process -= copy_size;

                if (current_bytes_in_buffer >= chunk_bytes) {
                    // 音频 x4 放大器
                    for (int i = 0; i < chunk_size; i++) {
                        int32_t sample = feed_buffer[i] * 4; 
                        if (sample > 32767) sample = 32767;
                        if (sample < -32768) sample = -32768;
                        feed_buffer[i] = (int16_t)sample;
                    }

                    // 喂给 AFE 降噪和唤醒引擎
                    afe_handle->feed(afe_data, feed_buffer);
                    current_bytes_in_buffer = 0; 
                }
            }
            vRingbufferReturnItem(sr_ringbuf, audio_data);
        }
    }
}

// ==========================================
// 识别任务：极简的 WakeNet 监听循环
// ==========================================
static void detect_task(void *arg) {
    ESP_LOGI(TAG, "🧠 AI 唤醒引擎正在监听 (内存自由模式)...");
    
    while (1) {
        // 从 AFE 获取处理结果
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) continue;

        // 如果检测到唤醒词
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "=================================");
            ESP_LOGI(TAG, "🚀 [成功] 识别到唤醒词：贾维斯！");
            ESP_LOGI(TAG, "=================================");
            
            // 💡 这里就是你未来大展拳脚的地方！
            // TODO 1: 播放一个滴答声，或者 AR 屏幕闪烁一下，提示用户“我在听”
            // TODO 2: 开启 WebSocket，把接下来的麦克风 PCM 数据发送给你的服务器
            // TODO 3: 接收服务器返回的识别结果
        }
    }
}

// ==========================================
// 外部调用：启动纯净唤醒大脑
// ==========================================
void start_jarvis_brain(void) {
    ESP_LOGI(TAG, "正在启动 AI 唤醒引擎 (仅 WakeNet)...");

    // 1. 初始化模型列表
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "❌ 模型初始化失败！");
        return;
    }

    // 2. 加载 WakeNet 唤醒词模型
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL); 
    if (wn_name == NULL) {
        ESP_LOGE(TAG, "❌ 分区内未找到 WakeNet 唤醒模型！");
        return;
    }
    ESP_LOGI(TAG, "🔍 成功加载唤醒模型: %s", wn_name);

    // (彻底移除了加载 MultiNet 的代码)

    // 3. 初始化 AFE (音频前端)
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->wakenet_model_name = wn_name; 
    
    // 既然我们去掉了 MultiNet，PSRAM 极度宽裕，这里用回默认配置即可
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM; 

    afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    
    // 门槛调低至 0.1，对口音极其友好
    afe_handle->set_wakenet_threshold(afe_data, 1, 0.1); 

    // 4. 启动任务
    xTaskCreatePinnedToCore(feed_audio_task, "voice_feed", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task,   "voice_detect", 8192, NULL, 5, NULL, 0);
    
    ESP_LOGI(TAG, "✅ 唤醒引擎启动完毕！系统内存现在极度安全。");
}