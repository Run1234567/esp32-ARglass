#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

// 乐鑫 ESP-SR 核心库 (唤醒词 WakeNet)
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

// ✨ 新增：乐鑫 ESP-SR 命令词识别库 (MultiNet)
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h" // ✨ 新增：V2.0+ 命令词管理专用头文件
#include "tts_app.h" 

static const char *TAG = "VOICE_APP";
static char *mn_model_name = NULL; // 记住命令模型的名字
extern RingbufHandle_t sr_ringbuf;
extern srmodel_list_t *esp_srmodel_init(const char *partition_label);

// ==========================================
// 全局句柄与状态标志
// ==========================================
// 1. AFE 降噪与唤醒句柄
static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

// 2. ✨ 新增：MultiNet 命令识别句柄
static esp_mn_iface_t *multinet_handle = NULL;
static model_iface_data_t *multinet_data = NULL;

// 3. 状态机标志：当前是在等唤醒，还是在听命令？
static bool is_listening_command = false; 

// ==========================================
// 数据搬运工：从 Ringbuf 捞水，放大后喂给 AFE
// ==========================================
static void feed_audio_task(void *arg) {
    int chunk_size = afe_handle->get_feed_chunksize(afe_data); 
    size_t chunk_bytes = chunk_size * sizeof(int16_t);
    
    int16_t *feed_buffer = malloc(chunk_bytes);
    size_t current_bytes_in_buffer = 0;
    size_t item_size = 0;
    int packet_count = 0;

    ESP_LOGI(TAG, "🎙️ AI 听觉搬运工已就绪，每次吞吐量: %d 采样点", chunk_size);

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
                    
                    // ✨ 核心修复：软件音频放大器 (放大 4 倍，解决声音小不识别的问题)
                    for (int i = 0; i < chunk_size; i++) {
                        int32_t sample = feed_buffer[i] * 4; 
                        // 防止爆音截断
                        if (sample > 32767) sample = 32767;
                        if (sample < -32768) sample = -32768;
                        feed_buffer[i] = (int16_t)sample;
                    }

                    // 放大后喂给引擎
                    afe_handle->feed(afe_data, feed_buffer);
                    current_bytes_in_buffer = 0; 
                    
                    if (++packet_count % 100 == 0) {
                        ESP_LOGD(TAG, "📥 已向 AFE 引擎输送 %d 帧音频", packet_count);
                    }
                }
            }
            vRingbufferReturnItem(sr_ringbuf, audio_data);
        }
    }
}
// ==========================================
// 识别任务：动态时分复用的双核状态机
// ==========================================
static void detect_task(void *arg) {
    ESP_LOGI(TAG, "🧠 AI 唤醒引擎正在监听 (极低内存模式)...");
    
    while (1) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) continue;

        // =====================================
        // 状态 1：日常待命，等待“贾维斯”唤醒
        // =====================================
        if (!is_listening_command) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "🚀 [成功] 识别到唤醒词：贾维斯！");
                
                // ✨ 核心内存魔术：被唤醒后，动态向 PSRAM 申请 2.5MB 内存加载理解大脑
                ESP_LOGI(TAG, "🔄 正在动态加载命令中枢，分配内存...");
                
                // 6000 代表留给用户 6 秒的时间说出命令
                multinet_data = multinet_handle->create(mn_model_name, 6000); 
                
                if (multinet_data != NULL) {
                    // 内存分配成功，开始注入指令
                    esp_mn_commands_clear();
                    esp_mn_commands_add(1, (char *)"da kai xian shi");
                    esp_mn_commands_add(2, (char *)"guan bi xian shi");
                    esp_mn_commands_add(3, (char *)"jin ru xiu mian");
                    esp_mn_commands_add(3, (char *)"jin ru xing xiu"); // 容错发音
                    esp_mn_commands_update();

                    is_listening_command = true; // 切换到命令倾听状态
                    ESP_LOGI(TAG, "👂 请在 6 秒内下达指令...");
                } else {
                    // 内存如果还是不够，优雅地报错，但不死机！
                    ESP_LOGE(TAG, "❌ 内存不足 (OOM)！命令大脑加载失败！");
                }
            }
        } 
        // =====================================
        // 状态 2：已唤醒，正在倾听并匹配拼音命令
        // =====================================
        else {
            esp_mn_state_t mn_state = multinet_handle->detect(multinet_data, res->data);

            if (mn_state == ESP_MN_STATE_DETECTED || mn_state == ESP_MN_STATE_TIMEOUT) {
                
                if (mn_state == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *mn_result = multinet_handle->get_results(multinet_data);
                    int command_id = mn_result->command_id[0];
                    ESP_LOGI(TAG, "🎯 识别到命令！ID: %d", command_id);
                    
                    // --- 在这里执行对应的动作 ---
                    switch (command_id) {
                        case 1: ESP_LOGI(TAG, "📺 执行: 打开显示"); break;
                        case 2: ESP_LOGI(TAG, "📺 执行: 关闭显示"); break;
                        case 3: ESP_LOGI(TAG, "💤 执行: 系统休眠"); break;
                        default: break;
                    }
                    
                } else {
                    ESP_LOGW(TAG, "⏱️ 倾听超时，未听到有效指令。");
                }
                
                // ✨ 核心内存魔术：用完立刻销毁，把 2.5MB 内存还给系统！
                ESP_LOGI(TAG, "🧹 销毁命令大脑，释放内存...");
                multinet_handle->destroy(multinet_data);
                multinet_data = NULL; // 清空指针，防止野指针崩溃
                
                is_listening_command = false; // 恢复待命
                ESP_LOGI(TAG, "🧠 恢复安静监听模式。");
            }
        }
    }
}

// ==========================================
// 外部调用：启动整个语音大脑
// ==========================================
void start_jarvis_brain(void) {
    ESP_LOGI(TAG, "正在启动 AI 语音双核引擎 (动态加载架构)...");

    // 1. 初始化模型列表
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "❌ 模型初始化失败！");
        return;
    }

    // 2. 加载 WakeNet 唤醒词模型 (常驻内存)
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL); 
    if (wn_name == NULL) {
        ESP_LOGE(TAG, "❌ 分区内未找到 WakeNet 唤醒模型！");
        return;
    }
    ESP_LOGI(TAG, "🔍 加载常驻唤醒模型: %s", wn_name);

    // 3. 寻找 MultiNet 命令词模型 (不立即加载，只记下名字)
    mn_model_name = esp_srmodel_filter(models, ESP_MN_CHINESE, NULL); 
    if (mn_model_name == NULL) {
        ESP_LOGE(TAG, "❌ 分区内未找到 MultiNet 命令模型！");
        return;
    }
    ESP_LOGI(TAG, "🔍 找到命令模型: %s (设为动态加载)", mn_model_name);

    // ✨ 初始化 MultiNet 接口句柄 (只搭框架，不占大内存)
    multinet_handle = esp_mn_handle_from_name(mn_model_name);

    // 4. 初始化 AFE (音频前端降噪)
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->wakenet_model_name = wn_name; 
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;

    afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    
    // 唤醒门槛调至 0.1，配合 x4 放大器，对中文口音极致友好
    afe_handle->set_wakenet_threshold(afe_data, 1, 0.1); 

    // 5. 启动双核驱动任务
    xTaskCreatePinnedToCore(feed_audio_task, "voice_feed", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task,   "voice_detect", 8192, NULL, 5, NULL, 0);
    
    ESP_LOGI(TAG, "✅ 语音双核大脑启动完毕！(内存安全模式)");
}