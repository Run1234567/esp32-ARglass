#include "tts_app.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h" 
#include "speaker_app.h" 
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // ✨ 引入队列
#include <string.h>
#include <ctype.h>  // 为了使用 ispunct 和 isspace

// ==========================================
// 🧠 内部辅助函数：判断并跳过中文标点 (UTF-8)
// ==========================================
static int skip_zh_punctuation(const char *str) {
    // 常见中文标点大全 (UTF-8编码，通常每个占3字节)
    const char *zh_puncs[] = {
        "，", "。", "！", "？", "：", "；", "、",
        "“", "”", "‘", "’", "（", "）",
        "【", "】", "《", "》", "…", "—", "～", NULL
    };
    
    for (int i = 0; zh_puncs[i] != NULL; i++) {
        int len = strlen(zh_puncs[i]);
        if (strncmp(str, zh_puncs[i], len) == 0) {
            return len; // 命中！返回这个标点占用的字节数
        }
    }
    return 0; // 不是中文标点
}

#define MOUNT_POINT "/sdcard"
static const char *TAG = "TTS_APP";

// --- 全局句柄 ---
static esp_tts_handle_t *tts_handle = NULL;
static uint8_t *model_data_in_psram = NULL;

// ✨ 我们只需要一个简单的队列，存放需要朗读的字符串指针
static QueueHandle_t tts_queue = NULL;

// ==========================================
// 1. 从 SD 卡加载模型 (保持你的原代码完全不变)
// ==========================================
esp_err_t load_tts_model_from_sd(const char* path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 无法打开 SD 卡上的模型文件: %s", path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    model_data_in_psram = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (model_data_in_psram == NULL) {
        ESP_LOGE(TAG, "❌ PSRAM 内存不足！");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(model_data_in_psram, 1, size, f);
    fclose(f);
    return ESP_OK;
}

// ==========================================
// 2. 核心任务：边转边播 (单兵作战，绝不死锁)
// ==========================================
void tts_main_task(void *pvParameters) {
    char *current_text = NULL;

    while (1) {
        // 1. 阻塞等待队列里的文字。如果没有字，任务在这里死等，绝对不占 CPU！
        if (xQueueReceive(tts_queue, &current_text, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(speaker_mutex, portMAX_DELAY); // 抢锁
            ESP_LOGI(TAG, "▶️ 开始合成并播放: %s", current_text);

            if (esp_tts_parse_chinese(tts_handle, current_text)) {
                int len[1] = {0};
                do {
                    // 2. 生成一小段 PCM 音频
                    short *pcm = esp_tts_stream_play(tts_handle, len, 4); 

                    if (pcm != NULL && len[0] > 0) {
                        // 3. 直接推给底层喇叭。
                        // 如果 I2S 正在忙，playSpeaker 会自动阻塞等待，把 CPU 让出去！
                        playSpeaker((uint8_t *)pcm, len[0] * 2);
                    }
                    
                    // 4. 强制喂狗，防止长句转换触发看门狗
                    vTaskDelay(pdMS_TO_TICKS(2)); 

                } while (len[0] > 0);
                
                // 清理引擎状态
                esp_tts_stream_reset(tts_handle);
            }
            xSemaphoreGive(speaker_mutex);
            // 5. 播完了，释放这块字符串内存
            free(current_text);
            current_text = NULL;
            ESP_LOGI(TAG, "✅ 播放完毕");
        }
    }
}

// ==========================================
// 3. 初始化引擎
// ==========================================
void init_tts_engine() {
    // A. 创建消息队列（最多排队 10 句话）
    tts_queue = xQueueCreate(10, sizeof(char *));

    // B. 加载模型
    if (load_tts_model_from_sd(MOUNT_POINT "/TTS_MO~1.DAT") != ESP_OK) return;

    // C. 初始化乐鑫引擎
    // C. 初始化乐鑫引擎
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)model_data_in_psram);
    tts_handle = esp_tts_create(voice);
    
    if (tts_handle == NULL) {
        ESP_LOGE(TAG, "❌ TTS 句柄创建失败！");
    } else {
        ESP_LOGI(TAG, "🚀 TTS 系统初始化成功");
        
        // D. 自动创建工作任务，扔到 Core 0，不跟你的网络/录音任务抢资源
        xTaskCreatePinnedToCore(tts_main_task, "tts_task", 8192, NULL, 5, NULL, 0);
    }
}
// ==========================================
// 🔊 4. 对外发声接口 (带自动标点净化)
// ==========================================
void tts_speak(const char *text) {
    if (text == NULL || tts_queue == NULL) return;
    
    // 1. 动态分配内存（去标点后只会更短，所以按原长度分配绝对够用）
    char *text_copy = (char *)malloc(strlen(text) + 1);
    if (text_copy == NULL) {
        ESP_LOGE(TAG, "❌ TTS 内存分配失败");
        return;
    }

    char *dst = text_copy;
    const char *src = text;

    // 2. 遍历原文本，无情击杀所有标点
    while (*src) {
        // 🎯 拦截 A：处理 ASCII 标点、控制符和多余空格 (如 , . ! ? \n \r)
        if (*src > 0 && *src < 127) {
            if (ispunct((unsigned char)*src) || iscntrl((unsigned char)*src) || isspace((unsigned char)*src)) {
                src++; // 遇到英文标点或空格，直接跳过
                continue;
            }
        }

        // 🎯 拦截 B：处理全角中文标点
        int zh_punc_len = skip_zh_punctuation(src);
        if (zh_punc_len > 0) {
            src += zh_punc_len; // 跳过这 3 个字节
            continue;
        }

        // ✅ 安全放行：正常的文字，拷贝过去
        *dst++ = *src++;
    }
    
    *dst = '\0'; // 重新安全封口

    // 3. 如果过滤完之后，发现这句话全是标点（变成空字符串了），就直接丢弃
    if (strlen(text_copy) == 0) {
        free(text_copy);
        return;
    }

    // 4. 扔进队列，如果满了就不等了，直接丢弃
    if (xQueueSend(tts_queue, &text_copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "⚠️ TTS 队伍太长，该句被丢弃");
        free(text_copy); // 发送失败也要记得释放内存
    } else {
        // ESP_LOGI(TAG, "🔊 净化后送入TTS: %s", text_copy); // 调试时可以打开看看效果
    }
}

// 兼容你之前的调用名
void jarvis_add_text(const char *new_text) {
    tts_speak(new_text);
}
