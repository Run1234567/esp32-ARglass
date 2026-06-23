/**
 * @file record_app.c
 * @brief 音频录音与照片保存模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 提供麦克风录音和摄像头拍照两大功能：
 *
 *   1. 麦克风录音 (WAV 格式)
 *      - 数据来源: main.c 中的 sd_ringbuf 环形缓冲区
 *      - 保存路径: /sdcard/录音/REC_001.wav, REC_002.wav, ...
 *      - 音频格式: 16kHz / 16-bit / 单声道 PCM
 *      - 自动生成标准 44 字节 WAV 文件头
 *      - 独立 FreeRTOS 任务从 RingBuffer 读取并写入 SD 卡
 *
 *   2. 拍照保存 (JPEG 格式)
 *      - 保存路径: /sdcard/拍照/IMG_001.jpg, IMG_002.jpg, ...
 *      - 自动递增编号，避免覆盖
 *
 *   3. 录音文件列表扫描
 *      - 扫描 /sdcard/录音/ 下的 .wav 文件
 *      - 通过 UART 发送给 UI
 *
 * 数据流:
 *   PDM麦克风 -> audio_hub_task -> sd_ringbuf -> record_task_worker -> SD卡
 *
 * WAV 文件格式:
 *   偏移 0:  "RIFF" (4字节)
 *   偏移 4:  文件大小-8 (4字节)
 *   偏移 8:  "WAVE" (4字节)
 *   偏移 12: "fmt " (4字节)
 *   偏移 16: fmt 块大小=16 (4字节)
 *   偏移 20: 音频格式=1(PCM) (2字节)
 *   偏移 22: 声道数=1 (2字节)
 *   偏移 24: 采样率=16000 (4字节)
 *   偏移 28: 字节率 (4字节)
 *   偏移 32: 块对齐 (2字节)
 *   偏移 34: 每样本位数=16 (2字节)
 *   偏移 36: "data" (4字节)
 *   偏移 40: 数据大小 (4字节)
 *   偏移 44: 音频数据开始...
 *
 * 依赖组件：
 *   - sd_card_app:  SD 卡挂载点 (MOUNT_POINT)
 *   - audio_app:    麦克风接口 (间接通过 RingBuffer)
 *   - esp32-camera: 摄像头接口
 *   - my_uart:      串口通信
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>          // mkdir, stat

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"  // 环形缓冲区

/* ==================== ESP-IDF 头文件 ==================== */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"        // 摄像头驱动

/* ==================== 项目组件头文件 ==================== */
#include "record_app.h"
#include "sd_card_app.h"       // MOUNT_POINT
#include "audio_app.h"
#include <dirent.h>            // 目录遍历
#include "my_uart.h"           // 串口通信

/**
 * @brief 外部引用：SD 卡录音专用环形缓冲区
 * 在 main.c 中创建，audio_hub_task 写入，本模块读取
 */
extern RingbufHandle_t sd_ringbuf;

static const char *TAG = "RECORD_APP";  // 日志标签

/* =====================================================================
 * 录音状态控制变量
 * ===================================================================== */
volatile bool is_recording = false;            // 录音状态标志
static FILE *record_file = NULL;               // 当前录音文件句柄
static uint32_t total_written_bytes = 0;       // 已写入的总字节数
static TaskHandle_t record_task_handle = NULL; // 录音任务句柄

/* =====================================================================
 * WAV 文件头生成函数
 * =====================================================================
 * @brief 在文件开头写入标准 44 字节 WAV 文件头
 *
 * 注意：由于录音开始时不知道最终数据大小，
 * 先写入一个占位头 (size=0)，录音结束后再回来更新正确的大小。
 *
 * @param f              文件指针
 * @param sample_rate    采样率 (Hz)
 * @param bits_per_sample 每采样位数
 * @param channels       声道数
 * @param audio_data_size 音频数据总字节数
 */
static void write_wav_header(FILE* f, uint32_t sample_rate, uint16_t bits_per_sample,
                              uint16_t channels, uint32_t audio_data_size) {
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);
    uint32_t chunk_size = audio_data_size + 36;  // 总大小 - 8

    fseek(f, 0, SEEK_SET);  // 回到文件开头

    /* ---- RIFF 头 ---- */
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* ---- fmt 子块 ---- */
    fwrite("fmt ", 1, 4, f);
    uint32_t subchunk1_size = 16;     // fmt 块大小
    uint16_t audio_format = 1;        // 1 = PCM
    fwrite(&subchunk1_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    /* ---- data 子块 ---- */
    fwrite("data", 1, 4, f);
    fwrite(&audio_data_size, 4, 1, f);
}

/* =====================================================================
 * 自动文件名分配 (录音)
 * =====================================================================
 * @brief 自动获取下一个不冲突的录音文件名
 *
 * 扫描 /sdcard/录音/ 目录，找到最小的可用编号。
 * 文件名格式: REC_001.wav, REC_002.wav, ...
 *
 * @param out_filepath 输出缓冲区
 * @param max_len      缓冲区大小
 */
static void get_next_filename(char *out_filepath, size_t max_len) {
    // 自动创建录音文件夹 (如果已存在则忽略)
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/录音", MOUNT_POINT);
    mkdir(dir_path, 0777);

    // 查找最小可用编号
    struct stat st;
    int file_index = 1;
    while (file_index <= 9999) {
        snprintf(out_filepath, max_len, "%s/录音/REC_%03d.wav", MOUNT_POINT, file_index);
        if (stat(out_filepath, &st) != 0) {
            break;  // 文件不存在，此编号可用
        }
        file_index++;
    }
    ESP_LOGI(TAG, "已自动分配录音文件名: %s", out_filepath);
}

/* =====================================================================
 * 录音工作线程
 * =====================================================================
 * @brief 独立任务：从 sd_ringbuf 读取音频数据并写入 SD 卡
 *
 * 工作流程：
 *   1. 从 sd_ringbuf 环形缓冲区读取音频数据
 *   2. 写入录音文件
 *   3. 收到停止信号后，更新 WAV 文件头并关闭文件
 *
 * 优先级: 5
 * 栈大小: 8192 字节
 */
static void record_task_worker(void *arg) {
    const uint32_t SAMPLE_RATE = 16000;
    size_t item_size;

    ESP_LOGI(TAG, "🎙️ 后台录音线程已启动，正在等待音频流...");

    /* ---- 录音主循环 ---- */
    while (is_recording) {
        // 从 SD 卡专属的 RingBuffer 提取数据，最多等 100ms
        void *sd_data = xRingbufferReceive(sd_ringbuf, &item_size, pdMS_TO_TICKS(100));

        if (sd_data != NULL) {
            if (record_file != NULL) {
                // 收到数据，写入 SD 卡
                fwrite(sd_data, 1, item_size, record_file);
                total_written_bytes += item_size;
            }
            // 归还 RingBuffer 内存
            vRingbufferReturnItem(sd_ringbuf, sd_data);
        }
    }

    /* ---- 录音结束：生成最终 WAV 文件 ---- */
    ESP_LOGI(TAG, "收到停止信号，正在生成最终 WAV 文件...");
    if (record_file != NULL) {
        // 回到文件开头，更新 WAV 头中的数据大小
        write_wav_header(record_file, SAMPLE_RATE, 16, 1, total_written_bytes);
        fclose(record_file);
        record_file = NULL;
    }

    ESP_LOGI(TAG, "✅ 录音安全结束！共写入: %lu 字节", total_written_bytes);

    record_task_handle = NULL;
    vTaskDelete(NULL);  // 删除自身任务
}

/* =====================================================================
 * 开始录音
 * =====================================================================
 * @brief 开始录制麦克风音频到 SD 卡
 *
 * 流程：
 *   1. 检查是否已在录音
 *   2. 自动分配文件名
 *   3. 创建文件并写入占位 WAV 头
 *   4. 设置录音标志 (audio_hub_task 会开始向 sd_ringbuf 写数据)
 *   5. 创建录音工作线程
 *
 * @return ESP_OK: 成功开始; ESP_ERR_INVALID_STATE: 已在录音; ESP_FAIL: 文件创建失败
 */
esp_err_t start_record(void) {
    if (is_recording) {
        ESP_LOGW(TAG, "警告：当前正在录音中，请先停止！");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 分配文件名 ---- */
    char filepath[64];
    get_next_filename(filepath, sizeof(filepath));

    /* ---- 创建录音文件 ---- */
    record_file = fopen(filepath, "wb");
    if (!record_file) {
        ESP_LOGE(TAG, "❌ 无法创建录音文件！请检查 SD 卡及目录权限。");
        return ESP_FAIL;
    }

    /* ---- 写入占位 WAV 头 (数据大小为 0，录音结束后更新) ---- */
    total_written_bytes = 0;
    write_wav_header(record_file, 16000, 16, 1, 0);

    /* ---- 启动录音 ---- */
    is_recording = true;  // 设置标志，audio_hub_task 开始向 sd_ringbuf 写数据
    /* 栈分配到 PSRAM */
    static StackType_t *rec_stack = NULL;
    static StaticTask_t rec_tcb;
    if (!rec_stack) {
        rec_stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    }
    if (rec_stack) {
        record_task_handle = xTaskCreateStaticPinnedToCore(record_task_worker, "rec_worker", 8192,
            NULL, 5, rec_stack, &rec_tcb, tskNO_AFFINITY);
    } else {
        xTaskCreate(record_task_worker, "rec_worker", 8192, NULL, 5, &record_task_handle);
    }

    return ESP_OK;
}

/* =====================================================================
 * 停止录音
 * =====================================================================
 * @brief 停止当前录音
 *
 * 仅设置 is_recording = false 标志，
 * record_task_worker 会在下次循环时检测到并退出。
 */
void stop_record(void) {
    if (is_recording) {
        ESP_LOGI(TAG, "发送停止录音信号...");
        is_recording = false;  // 录音线程会在下次循环退出
    } else {
        ESP_LOGW(TAG, "当前并没有在录音。");
    }
}

/* =====================================================================
 * 自动文件名分配 (拍照)
 * =====================================================================
 * @brief 自动获取下一个不冲突的照片文件名
 *
 * @param out_filepath 输出缓冲区
 * @param max_len      缓冲区大小
 */
static void get_next_img_filename(char *out_filepath, size_t max_len) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/拍照", MOUNT_POINT);
    mkdir(dir_path, 0777);  // 自动创建拍照文件夹

    struct stat st;
    int file_index = 1;
    while (file_index <= 9999) {
        snprintf(out_filepath, max_len, "%s/拍照/IMG_%03d.jpg", MOUNT_POINT, file_index);
        if (stat(out_filepath, &st) != 0) {
            break;  // 文件不存在，此编号可用
        }
        file_index++;
    }
}

/* =====================================================================
 * 拍照并保存
 * =====================================================================
 * @brief 从摄像头拍摄一张照片并保存到 SD 卡
 *
 * 流程：
 *   1. 从摄像头获取一帧 JPEG 图像
 *   2. 自动分配不冲突的文件名
 *   3. 写入 SD 卡
 *   4. 释放帧缓冲 (极其重要！)
 *
 * @return ESP_OK: 成功; ESP_FAIL: 摄像头或文件系统错误
 */
esp_err_t take_photo_and_save(void) {
    ESP_LOGI(TAG, "📸 准备抓拍 UXGA 高清图像...");

    /* ---- 步骤1: 获取一帧图像 ---- */
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "❌ 摄像头获取画面失败！请检查硬件或引脚。");
        return ESP_FAIL;
    }

    /* ---- 步骤2: 分配文件名 ---- */
    char filepath[64];
    get_next_img_filename(filepath, sizeof(filepath));

    /* ---- 步骤3: 写入 SD 卡 ---- */
    FILE *file = fopen(filepath, "wb");
    if (!file) {
        ESP_LOGE(TAG, "❌ 无法在 SD 卡创建照片文件！");
        // 即使失败，也要归还帧缓冲给摄像头！
        esp_camera_fb_return(fb);
        return ESP_FAIL;
    }

    fwrite(fb->buf, 1, fb->len, file);
    fclose(file);

    ESP_LOGI(TAG, "✅ 照片成功保存到: %s (大小: %zu KB)", filepath, fb->len / 1024);

    /* ---- 步骤4: 归还帧缓冲 ---- */
    // 极其重要：不归还会导致下次拍照时内存溢出
    esp_camera_fb_return(fb);

    return ESP_OK;
}

/* =====================================================================
 * 录音文件列表扫描
 * =====================================================================
 * @brief 扫描 /sdcard/录音/ 目录，将所有 .wav 文件名发送给 UI
 *
 * 通信协议：
 *   1. "CMD:CLEAR_LIST"  - 清空 UI 列表
 *   2. "REC_FILE:文件名" - 每个录音文件
 *   3. "CMD:LIST_END"    - 列表发送完毕
 */
void scan_and_send_record_list(void) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/录音", MOUNT_POINT);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE("RECORD_APP", "❌ 无法打开录音文件夹");
        my_uart_send("CMD:CLEAR_LIST\r\n");
        vTaskDelay(pdMS_TO_TICKS(20));
        my_uart_send("CMD:LIST_END\r\n");
        return;
    }

    // 通知 UI 清空列表
    my_uart_send("CMD:CLEAR_LIST\r\n");
    vTaskDelay(pdMS_TO_TICKS(20));

    struct dirent *entry;
    char uart_buf[512];

    // 遍历所有 .wav 文件
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        // 检查扩展名是否为 .wav (不区分大小写)
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".wav") == 0) {
            snprintf(uart_buf, sizeof(uart_buf), "REC_FILE:%s\r\n", entry->d_name);
            my_uart_send(uart_buf);
            vTaskDelay(pdMS_TO_TICKS(20));  // 延时防止 UART 溢出
        }
    }
    closedir(dir);

    // 通知 UI 列表发送完毕
    my_uart_send("CMD:LIST_END\r\n");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI("RECORD_APP", "📁 录音列表发送完毕");
}
