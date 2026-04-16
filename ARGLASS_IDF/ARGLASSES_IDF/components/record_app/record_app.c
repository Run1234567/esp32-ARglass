#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// 引入自身头文件和其他底层模块的头文件
#include "record_app.h"
#include "sd_card_app.h" 
#include "audio_app.h"
#include "esp_camera.h"

static const char *TAG = "RECORD_APP";

// ==========================================
// ⚙️ 录音状态全局控制变量
// ==========================================
static volatile bool is_recording = false;   
static FILE *record_file = NULL;             
static uint32_t total_written_bytes = 0;     
static TaskHandle_t record_task_handle = NULL; 

// ==========================================
// 🎵 内部函数：生成标准 WAV 文件头
// ==========================================
static void write_wav_header(FILE* f, uint32_t sample_rate, uint16_t bits_per_sample, uint16_t channels, uint32_t audio_data_size) {
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);
    uint32_t chunk_size = audio_data_size + 36;

    fseek(f, 0, SEEK_SET); 
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1;
    fwrite(&subchunk1_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    
    fwrite("data", 1, 4, f);
    fwrite(&audio_data_size, 4, 1, f);
}

// ==========================================
// 🔍 内部函数：自动获取下一个不冲突的文件名
// ==========================================
static void get_next_filename(char *out_filepath, size_t max_len) {
    struct stat st;
    int file_index = 1;
    
    while (file_index <= 9999) { 
        snprintf(out_filepath, max_len, "%s/REC_%03d.wav", MOUNT_POINT, file_index);
        if (stat(out_filepath, &st) != 0) {
            break; 
        }
        file_index++;
    }
    ESP_LOGI(TAG, "已自动分配录音文件名: %s", out_filepath);
}

// ==========================================
// 👷 内部任务：独立录音线程
// ==========================================
static void record_task_worker(void *arg) {
    const uint32_t SAMPLE_RATE = 16000;
    const size_t BUFFER_SAMPLES = 512; 
    int16_t audio_buffer[BUFFER_SAMPLES];

    ESP_LOGI(TAG, "🎙️ 后台录音线程已启动，正在实时写入...");

    while (is_recording) {
        size_t bytes_read = readAudio(audio_buffer, BUFFER_SAMPLES);
        if (bytes_read > 0 && record_file != NULL) {
            fwrite(audio_buffer, 1, bytes_read, record_file);
            total_written_bytes += bytes_read;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); 
        }
    }

    ESP_LOGI(TAG, "收到停止信号，正在生成最终 WAV 文件...");
    if (record_file != NULL) {
        write_wav_header(record_file, SAMPLE_RATE, 16, 1, total_written_bytes);
        fclose(record_file);
        record_file = NULL;
    }
    
    ESP_LOGI(TAG, "✅ 录音安全结束！共写入: %lu 字节", total_written_bytes);
    
    record_task_handle = NULL;
    vTaskDelete(NULL); 
}

// ==========================================
// 🟢 对外接口：开始录音 (已融合自动命名)
// ==========================================
esp_err_t start_record(void) {
    if (is_recording) {
        ESP_LOGW(TAG, "警告：当前正在录音中，请先停止！");
        return ESP_ERR_INVALID_STATE;
    }

    char filepath[64];
    get_next_filename(filepath, sizeof(filepath));
    
    record_file = fopen(filepath, "wb");
    if (!record_file) {
        ESP_LOGE(TAG, "❌ 无法创建录音文件！");
        return ESP_FAIL;
    }

    total_written_bytes = 0;
    write_wav_header(record_file, 16000, 16, 1, 0); 
    
    is_recording = true;
    xTaskCreate(record_task_worker, "rec_worker", 8192, NULL, 5, &record_task_handle);
    
    return ESP_OK;
}

// ==========================================
// 🔴 对外接口：结束录音
// ==========================================
void stop_record(void) {
    if (is_recording) {
        ESP_LOGI(TAG, "发送停止录音信号...");
        is_recording = false; 
    } else {
        ESP_LOGW(TAG, "当前并没有在录音。");
    }
}

// ==========================================
// 🔍 内部函数：自动获取下一个不冲突的照片名
// ==========================================
static void get_next_img_filename(char *out_filepath, size_t max_len) {
    struct stat st;
    int file_index = 1;
    
    while (file_index <= 9999) { 
        // 生成如 /sdcard/IMG_001.jpg 的名字
        snprintf(out_filepath, max_len, "%s/IMG_%03d.jpg", MOUNT_POINT, file_index);
        if (stat(out_filepath, &st) != 0) {
            break; // 找到空闲名字
        }
        file_index++;
    }
}

// ==========================================
// 📸 对外接口：一键拍照并存入 SD 卡
// ==========================================
esp_err_t take_photo_and_save(void) {
    ESP_LOGI(TAG, "📸 准备抓拍 UXGA 高清图像...");
    
    // 1. 从摄像头获取一帧图像 (得到的是已经压缩好的 JPEG 二进制流)
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "❌ 摄像头获取画面失败！请检查硬件或引脚。");
        return ESP_FAIL;
    }

    // 2. 自动分配一个不冲突的 JPG 文件名
    char filepath[64];
    get_next_img_filename(filepath, sizeof(filepath));

    // 3. 在 SD 卡上创建并打开文件
    FILE *file = fopen(filepath, "wb");
    if (!file) {
        ESP_LOGE(TAG, "❌ 无法在 SD 卡创建照片文件！");
        // 🚨 极其重要：即使失败，也要把内存还给摄像头！
        esp_camera_fb_return(fb); 
        return ESP_FAIL;
    }

    // 4. 将摄像头的二进制流全速灌入 SD 卡
    fwrite(fb->buf, 1, fb->len, file);
    fclose(file);

    ESP_LOGI(TAG, "✅ 照片成功保存到: %s (大小: %zu KB)", filepath, fb->len / 1024);

    // 5. 用完之后，务必将内存缓存还给摄像头，否则拍第二张时会内存溢出
    esp_camera_fb_return(fb);
    
    return ESP_OK;
}

