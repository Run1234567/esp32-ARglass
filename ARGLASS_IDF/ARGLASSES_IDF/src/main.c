#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h" // ? 引入信号量的头文件

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h" // ? 引入原生 WebSocket 客户端
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h" // 乐鑫内置的中文字库定义
#include <stdlib.h> // 提供 malloc 和 free
#include <dirent.h>

// 关键：对应 CMakeLists.txt 中添加的二进制文件符号
extern const uint8_t esp_tts_voice_data_xiaole_dat_start[] asm("_binary_esp_tts_voice_data_xiaole_dat_start");

// 全局句柄，初始化一次，到处使用
esp_tts_handle_t *tts_handle = NULL;

// 引入我们的四大底层组件
#include "wifi_app.h"
#include "camera_app.h"
#include "audio_app.h"
#include "speaker_app.h"
#include "esp_camera.h"
#include "sd_card_app.h" // ? 加上这句！引入 SD 卡模块
#include "my_uart.h"    // ? 引入串口模块 (彻底替换了 app_mqtt.h)
#include "record_app.h" // ? 加上这句！引入录音模块
#include "tts_app.h" // ? 加上这句！引入 TTS 模块
#include "music_app.h" // ? 加上这句！引入音乐播放器模块
// ? 引入环形缓冲区和数学库
#include "freertos/ringbuf.h" 
#include <math.h>
#include "voice_app.h" // ? 新增：引入你的 AI 语音大脑头文件
// 全局音频分发缓冲区
RingbufHandle_t ws_ringbuf = NULL;
RingbufHandle_t sd_ringbuf = NULL;
RingbufHandle_t yin_ringbuf = NULL; // ? 新增：专为测音调准备的缓冲池
RingbufHandle_t sr_ringbuf = NULL; // ? 新增：AI 语音识别专属缓冲池
static const char *TAG = "J.A.R.V.I.S";

volatile bool send_noise_data = false;
volatile bool send_pitch_data = false;

// ==========================================
// ?? 您的专属配置 ??
// ==========================================
const char* websocket_url = "ws://124.220.224.189:8765/";

// ==========================================
// ? 全局状态与句柄
// ==========================================
esp_websocket_client_handle_t ws_client;

SemaphoreHandle_t speaker_mutex = NULL;
SemaphoreHandle_t next_page_sem = NULL;
// ==========================================
// ? 拍照并发送
// ==========================================
void captureAndSend(void) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
        // 通过 WebSocket 发送二进制图片数据
        esp_websocket_client_send_bin(ws_client, (const char *)fb->buf, fb->len, portMAX_DELAY);
        ESP_LOGI(TAG, "? 收到指令，照片已即时发送 (%zu bytes)", fb->len);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "? 摄像头采集失败");
    }
}
// ==========================================
// ? 分贝 (噪声) 计算函数
// ==========================================
float calculate_decibel(int16_t *buffer, size_t samples) {
    if (samples == 0 || buffer == NULL) return 0.0f;
    int64_t sum_squares = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t val = buffer[i];
        sum += val;
        sum_squares += (int64_t)val * val;
    }
    float mean = (float)sum / samples;
    float mean_square = (float)sum_squares / samples - (mean * mean); // 滤除直流偏置
    if (mean_square <= 0.0f) return 0.0f;
    
    float rms = sqrtf(mean_square);
    if (rms < 1.0f) rms = 1.0f; 
    
    float dbfs = 20.0f * log10f(rms / 32768.0f);
    return dbfs + 90.0f; // 90.0f 是硬件校准偏移量，可根据实际麦克风微调
}
// ==========================================
// ? 频率转音调名称辅助函数
// ==========================================
const char* note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void print_pitch_from_freq(float freq) {
    if (freq <= 50.0f || freq >= 2000.0f) return; // 过滤太低或太高（人声/常规乐器外）的杂音

    // 计算 MIDI 音符编号
    float midi_float = 12.0f * log2f(freq / 440.0f) + 69.0f;
    int midi_note = (int)roundf(midi_float);

    int note_index = midi_note % 12;
    int octave = (midi_note / 12) - 1;
    float cents = (midi_float - midi_note) * 100.0f;

    ESP_LOGI("PITCH", "? 频率: %5.1f Hz -> 音高: %2s%d (偏差: %4.1f cents)", 
             freq, note_names[note_index], octave, cents);
}

// ==========================================
// ? YIN 时域自相关算法 (高精度提取基频)
// ==========================================
float calculate_pitch_yin(int16_t *buffer, size_t buffer_size, int sample_rate) {
    int half_size = buffer_size / 2;
    
    // 分配在堆内存，防止撑爆 Task 栈
    float *yin_buffer = (float *)malloc(half_size * sizeof(float));
    if (yin_buffer == NULL) return 0.0f;

    // 1. 差值函数
    for (int tau = 0; tau < half_size; tau++) {
        yin_buffer[tau] = 0;
        for (int i = 0; i < half_size; i++) {
            float delta = (float)buffer[i] - (float)buffer[i + tau];
            yin_buffer[tau] += delta * delta;
        }
    }

    // 2. 累积平均归一化差值
    yin_buffer[0] = 1;
    float running_sum = 0;
    for (int tau = 1; tau < half_size; tau++) {
        running_sum += yin_buffer[tau];
        yin_buffer[tau] *= tau / running_sum;
    }

    // 3. 寻找绝对阈值 (阈值设为 0.15)
    int tau_estimate = -1;
    for (int tau = 2; tau < half_size; tau++) {
        if (yin_buffer[tau] < 0.15f) {
            while (tau + 1 < half_size && yin_buffer[tau + 1] < yin_buffer[tau]) {
                tau++;
            }
            tau_estimate = tau;
            break;
        }
    }

    if (tau_estimate == -1) {
        free(yin_buffer);
        return 0.0f; 
    }

    // 4. 抛物线插值 (提升精度)
    float better_tau = tau_estimate;
    if (tau_estimate > 0 && tau_estimate < half_size - 1) {
        float s0 = yin_buffer[tau_estimate - 1];
        float s1 = yin_buffer[tau_estimate];
        float s2 = yin_buffer[tau_estimate + 1];
        better_tau += (s2 - s0) / (2.0f * (2.0f * s1 - s2 - s0));
    }

    free(yin_buffer);
    return (float)sample_rate / better_tau;
}
// ==========================================
// ? 音频分发中心 (Audio Hub)
// ==========================================
void audio_hub_task(void *pvParameters) {
    const size_t samples = 512;
    int16_t audioBuffer[samples];

    static TickType_t last_send_time = 0;

    ESP_LOGI("AUDIO_HUB", "?? 麦克风核心采集枢纽已启动");

    while (1) {
        size_t bytesRead = readAudio(audioBuffer, samples);
        
        if (bytesRead > 0) {
            // 1. 算噪音
            float db_value = calculate_decibel(audioBuffer, samples);

            if (send_noise_data) {
                TickType_t current_time = xTaskGetTickCount();
                if (current_time - last_send_time >= pdMS_TO_TICKS(100)) {
                    char db_cmd[16];
                    snprintf(db_cmd, sizeof(db_cmd), "DB:%d", (int)db_value);
                    my_uart_send(db_cmd);
                    last_send_time = current_time;
                }
            }

            // 2. 发给 WebSocket
            if (ws_ringbuf != NULL && esp_websocket_client_is_connected(ws_client)) {
                xRingbufferSend(ws_ringbuf, audioBuffer, bytesRead, 0); 
            }
            
            // 3. 发给 SD 卡录音
            extern volatile bool is_recording;
            if (sd_ringbuf != NULL && is_recording) {
                xRingbufferSend(sd_ringbuf, audioBuffer, bytesRead, 0);
            }

            // ? 4. 发给 YIN 测音任务
            // 如果只有声音大于 50dB 才发，可以省下一大笔计算资源
            if (yin_ringbuf != NULL && db_value > 35.0f) {
                // 等待时间设为 0。如果 YIN 任务算得太慢导致池子满了，
                // Hub 会直接丢弃这帧数据，绝不卡死自己！
                xRingbufferSend(yin_ringbuf, audioBuffer, bytesRead, 0);
            }
            // ? 4. 无情地把声音灌给 AI 引擎
            if (sr_ringbuf != NULL) {
                // 等待时间设为 0。AI 处理不过来自动丢弃，绝不卡死 Hub
                xRingbufferSend(sr_ringbuf, audioBuffer, bytesRead, 0); 
            }
        }
    }
}
// ==========================================
// ? WebSocket 事件回调 (替代原先的 webSocketEvent)
// ==========================================
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "? 已连接到基站");
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "? 连接断开，底层尝试重连中...");
            break;
            
        case WEBSOCKET_EVENT_DATA:
            // op_code == 1 表示收到的是文本消息 (TEXT)
            if (data->op_code == 1) {
                // 判断是否是 CAPTURE 指令 (使用 strncmp 防止越界)
                if (data->data_len >= 7 && strncmp((char *)data->data_ptr, "CAPTURE", 7) == 0) {
                    ESP_LOGI(TAG, "? 收到拍照请求...");
                    captureAndSend();
                }
            } 
            // op_code == 2 表示收到的是二进制流 (BIN)，即 Python 发来的音频 PCM 数据
            else if (data->op_code == 2) {
                playSpeaker((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;
    }
}

// ==========================================
// ? WebSocket 推流专员 (只管发，不碰硬件)
// ==========================================
void audio_tx_task(void *pvParameters) {
    size_t item_size;
    while (1) {
        if (esp_websocket_client_is_connected(ws_client)) {
            // 从缓冲池提取数据，等待时间 portMAX_DELAY (没有数据就乖乖休眠，不占CPU)
            void *tx_data = xRingbufferReceive(ws_ringbuf, &item_size, portMAX_DELAY);
            if (tx_data != NULL) {
                esp_websocket_client_send_bin(ws_client, (const char*)tx_data, item_size, portMAX_DELAY);
                // 用完必须归还内存给 RingBuffer
                vRingbufferReturnItem(ws_ringbuf, tx_data);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}
// ==========================================
// ? 独立任务：小说读取与发送专员
// ==========================================
void novel_read_task(void *pvParameters) {
    ESP_LOGI("NOVEL_TASK", "小说读取子任务已启动，正在待命...");

    while (1) {
        // ? 核心：在这里等信号，不干活时完全不占 CPU
        if (xSemaphoreTake(next_page_sem, portMAX_DELAY) == pdTRUE) {
            
            ESP_LOGI("NOVEL_TASK", "收到翻页信号，开始工作...");
            
            // 执行具体的读卡和 MQTT 发送逻辑
            test_read_novel_next_chunk();
            
            ESP_LOGI("NOVEL_TASK", "任务完成，继续待命。");
        }
    }
    
    // 理论上永远不会走到这里，但作为好习惯，任务退出要删除自己
    vTaskDelete(NULL);
}
// ==========================================
// ? 独立任务：高精度绝对音感提取 (YIN) (已修复死锁漏洞)
// ==========================================
void yin_pitch_task(void *pvParameters) {
    const size_t target_samples = 2048; // 每次攒够 2048 个点算一次
    int16_t *accum_buffer = malloc(target_samples * sizeof(int16_t));
    size_t current_count = 0;
    size_t item_size;

    ESP_LOGI("YIN_TASK", "? 独立测音任务已启动，正在后台监听...");

    while (1) {
        // 从专属缓冲池里捞数据
        void *audio_data = xRingbufferReceive(yin_ringbuf, &item_size, portMAX_DELAY);
        
        if (audio_data != NULL) {
            size_t samples_received = item_size / sizeof(int16_t);

            // ? 修复核心：计算水池还能装多少，哪怕溢出了也只取需要的部分填满
            size_t space_left = target_samples - current_count;
            size_t samples_to_copy = (samples_received < space_left) ? samples_received : space_left;

            memcpy(&accum_buffer[current_count], audio_data, samples_to_copy * sizeof(int16_t));
            current_count += samples_to_copy;

            // 用完必须把内存还给 RingBuffer
            vRingbufferReturnItem(yin_ringbuf, audio_data);

            // 水池满了，开始高强度计算！
            if (current_count >= target_samples) {
                // 调用 YIN 算法
                float exact_freq = calculate_pitch_yin(accum_buffer, target_samples, 16000);
                
                if (exact_freq > 20.0f) {
                    //ESP_LOGI("PITCH_RESULT", "? 抓到声音了！当前主频率: %.2f Hz", exact_freq);
                    print_pitch_from_freq(exact_freq);

                    if (send_pitch_data) {
                        static TickType_t last_p_time = 0;
                        if (xTaskGetTickCount() - last_p_time > pdMS_TO_TICKS(100)) {
                            char p_cmd[20];
                            snprintf(p_cmd, sizeof(p_cmd), "PH:%.1f", exact_freq);
                            my_uart_send(p_cmd);
                            last_p_time = xTaskGetTickCount();
                        }
                    }
                }
                else {
                    // 如果环境全是呼呼的风声底噪，YIN 算法会返回 0，这句一定会打印！
                    // ESP_LOGW("PITCH_RESULT", "? 声音杂乱无固定周期 (非乐音)");
                }
                
                // 清空水池，准备攒下一波
                current_count = 0; 
            }
        }
    }
    
    free(accum_buffer);
    vTaskDelete(NULL);
}
void app_main(void) {
     vTaskDelay(pdMS_TO_TICKS(3000)); 
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    speaker_mutex = xSemaphoreCreateMutex();
    
    if (speaker_mutex == NULL) {
        ESP_LOGE("MAIN", "? 致命错误：喇叭互斥锁创建失败！");
        return; // 如果锁没造出来，后面的系统就别跑了
    }
    // 调用模块暴露的接口
    if (init_sd_card() == ESP_OK) {
        test_sd_card_read_write();
    } else {
        ESP_LOGE(TAG, "?? SD 卡模块异常，跳过后续依赖任务...");
    }
    // 创建环形缓冲区，每个分配 10KB 大小，足以缓冲数个音频帧
    // NOSPLIT 类型保证每次存入的一帧数据被完整取出
    ws_ringbuf = xRingbufferCreate(10240, RINGBUF_TYPE_NOSPLIT);
    sd_ringbuf = xRingbufferCreate(10240, RINGBUF_TYPE_NOSPLIT);
    yin_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_NOSPLIT);
    sr_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_NOSPLIT);
    if (ws_ringbuf == NULL || sd_ringbuf == NULL) {
        ESP_LOGE(TAG, "? 致命错误：音频环形缓冲区创建失败！");
        return;
    }
    // ==========================================

    // 2. 初始化网络与三大硬件
    wifi_init_sta();
    
    ESP_LOGI(TAG, "? 等待 WiFi 分配 IP...");
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    initCamera();
    initAudio();
    initSpeaker();
    // 2. 初始化引擎
    init_tts_engine();
    start_jarvis_brain();
    // 3. 运行业务
    
    next_page_sem = xSemaphoreCreateBinary();
    if (next_page_sem == NULL) {
        ESP_LOGE(TAG, "致命错误：信号量创建失败，内存不足！");
        return; 
    }
    // 3. 配置并启动 WebSocket 客户端
    esp_websocket_client_config_t websocket_cfg = {
        .uri = websocket_url,
        .reconnect_timeout_ms = 5000, // 断线自动 5 秒重连
    };
    ws_client = esp_websocket_client_init(&websocket_cfg);
    
    // 注册事件回调监听器
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
    
    // 启动连接
    esp_websocket_client_start(ws_client);
    my_uart_init();
    take_photo_to_PZ_folder();
    // 4. 开启独立线程：无情地抓取麦克风数据发给基站
// ? 核心救命代码：强制绑定到 Core 1 (参数最后的 1) ?
    
    // 1. 麦克风核心采集任务 (代替硬件读取硬件)
    xTaskCreatePinnedToCore(audio_hub_task, "audio_hub", 8192, NULL, 5, NULL, 1);
    
    // 2. WebSocket 发送任务 
    xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 8192, NULL, 4, NULL, 1);
    
    // 3. 小说读取任务
    xTaskCreatePinnedToCore(novel_read_task, "novel_task", 4096 * 2, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(yin_pitch_task, "yin_task", 8192, NULL, 3, NULL, 1);
    tts_speak("贾维斯系统已启动。主脑连接成功，正在等待指令。");
   // 主线程可在此挂起
    while(1) {
        my_uart_send("TEMP:52");
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }
}
