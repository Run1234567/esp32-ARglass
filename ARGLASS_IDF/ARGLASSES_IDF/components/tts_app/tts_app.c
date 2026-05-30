#include "tts_app.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h" 
#include "speaker_app.h" 
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "my_uart.h" 
#include <string.h>
#include <ctype.h>

static const char *TAG = "TTS_APP";
#define MOUNT_POINT "/sdcard"

// ==========================================
// ⚙️ 全局控制变量
// ==========================================
volatile int global_tts_speed = 4;        // 默认语速
volatile bool is_reading_active = false;  // 是否处于连续读小说模式
volatile bool force_stop_tts = false;     // 强行打断说话标志

static esp_tts_handle_t *tts_handle = NULL;
static uint8_t *model_data_in_psram = NULL;
static QueueHandle_t tts_queue = NULL;

// ==========================================
// 🧠 内部辅助函数：判断并跳过中文标点
// ==========================================
static int skip_zh_punctuation(const char *str) {
    const char *zh_puncs[] = {
        "，", "。", "！", "？", "：", "；", "、",
        "“", "”", "‘", "’", "（", "）",
        "【", "】", "《", "》", "…", "—", "～", NULL
    };
    for (int i = 0; zh_puncs[i] != NULL; i++) {
        int len = strlen(zh_puncs[i]);
        if (strncmp(str, zh_puncs[i], len) == 0) return len; 
    }
    return 0; 
}

// 设置语速
void tts_set_speed(int speed) {
    global_tts_speed = speed;
}

// 打断说话并清空队列
void stop_tts_reading(void) {
    is_reading_active = false;
    force_stop_tts = true; 
    char *temp;
    // 把排队的句子全部扔进垃圾桶
    while(xQueueReceive(tts_queue, &temp, 0) == pdTRUE) {
        free(temp);
    }
}

// 加载模型
esp_err_t load_tts_model_from_sd(const char* path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 无法打开模型文件: %s", path);
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
// 🎙️ 核心任务：边转边播 (已修复哑巴Bug)
// ==========================================
void tts_main_task(void *pvParameters) {
    char *current_text = NULL;

    while (1) {
        if (xQueueReceive(tts_queue, &current_text, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(speaker_mutex, portMAX_DELAY); 

            force_stop_tts = false; // 每次拿到新句子，允许发声

            // ✨ 如果是在读小说，把当前这句话发给屏幕显示
            if (is_reading_active) {
                char *uart_buf = malloc(strlen(current_text) + 10);
                if (uart_buf) {
                    sprintf(uart_buf, "NOV:%s", current_text);
                    my_uart_send(uart_buf);
                    free(uart_buf);
                }
            }

            // ✨ 不管是不是小说模式，只要拿到字就必须开口！
            ESP_LOGI(TAG, "▶️ 开始合成并播放: %s", current_text);

            if (esp_tts_parse_chinese(tts_handle, current_text)) {
                int len[1] = {0};
                do {
                    // 如果收到打断指令 (比如用户退出了小说界面)，立刻闭嘴退出循环
                    if (force_stop_tts) break; 
                    
                    short *pcm = esp_tts_stream_play(tts_handle, len, global_tts_speed); 
                    if (pcm != NULL && len[0] > 0) {
                        playSpeaker((uint8_t *)pcm, len[0] * 2);
                    }
                    vTaskDelay(pdMS_TO_TICKS(2)); 
                } while (len[0] > 0);
                
                esp_tts_stream_reset(tts_handle);
            }
            xSemaphoreGive(speaker_mutex); // 归还喇叭
            
            free(current_text);
            current_text = NULL;

            // ✨ 如果在读小说模式下，这句读完了且队伍空了，立刻发信号去 SD 卡要新字
            if (is_reading_active && !force_stop_tts && uxQueueMessagesWaiting(tts_queue) == 0) {
                extern SemaphoreHandle_t next_page_sem;
                if (next_page_sem != NULL) {
                    xSemaphoreGive(next_page_sem);
                }
            }
        }
    }
}

// ==========================================
// 🚀 初始化引擎
// ==========================================
void init_tts_engine() {
    tts_queue = xQueueCreate(10, sizeof(char *));

    // ✨ 这里用的是真名，确保和 SD 卡里放的文件一模一样！
    if (load_tts_model_from_sd(MOUNT_POINT "/esp_tts_voice_data_xiaole.dat") != ESP_OK) return;

    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)model_data_in_psram);
    tts_handle = esp_tts_create(voice);
    
    if (tts_handle == NULL) {
        ESP_LOGE(TAG, "❌ TTS 句柄创建失败！");
    } else {
        ESP_LOGI(TAG, "🚀 TTS 系统初始化成功");
        
        // ✨ 暴增到 32KB 内存，它这辈子都不会再爆栈了！
        xTaskCreatePinnedToCore(tts_main_task, "tts_task", 32768, NULL, 5, NULL, 0);
    }
}

// ==========================================
// 📢 发声接口 (自带净化和切句，防过载)
// ==========================================
void tts_speak(const char *text) {
    if (text == NULL || tts_queue == NULL) return;
    
    const int MAX_CHUNK_BYTES = 90; 
    char chunk_buf[MAX_CHUNK_BYTES + 4]; 
    int chunk_len = 0;

    const char *src = text;

    while (*src) {
        int char_bytes = 1;
        unsigned char c = (unsigned char)*src;
        if (c < 0x80) char_bytes = 1;
        else if (c < 0xE0) char_bytes = 2;
        else if (c < 0xF0) char_bytes = 3;
        else char_bytes = 4;

        int is_punc = 0;
        int zh_punc_len = 0;

        if (char_bytes == 1 && (ispunct(c) || iscntrl(c) || isspace(c))) {
            is_punc = 1;
        } else if (char_bytes >= 3) {
            zh_punc_len = skip_zh_punctuation(src);
            if (zh_punc_len > 0) is_punc = 1;
        }

        if (is_punc) {
            if (chunk_len > 0) {
                chunk_buf[chunk_len] = '\0';
                char *text_copy = strdup(chunk_buf);
                if (text_copy) {
                    if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) free(text_copy);
                }
                chunk_len = 0; 
            }
            src += (zh_punc_len > 0) ? zh_punc_len : 1; 
        } else {
            if (chunk_len + char_bytes > MAX_CHUNK_BYTES) {
                chunk_buf[chunk_len] = '\0';
                char *text_copy = strdup(chunk_buf);
                if (text_copy) {
                    if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) free(text_copy);
                }
                chunk_len = 0;
            }

            for (int i = 0; i < char_bytes; i++) {
                chunk_buf[chunk_len++] = src[i];
            }
            src += char_bytes;
        }
    }

    if (chunk_len > 0) {
        chunk_buf[chunk_len] = '\0';
        char *text_copy = strdup(chunk_buf);
        if (text_copy) {
            if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) free(text_copy);
        }
    }
}

// 兼容你之前的调用名
void jarvis_add_text(const char *new_text) {
    tts_speak(new_text);
}