#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h" // ? 引入原生 WebSocket 客户端

// 引入我们的四大底层组件
#include "wifi_app.h"
#include "camera_app.h"
#include "audio_app.h"
#include "speaker_app.h"
#include "esp_camera.h"
#include "sd_card_app.h" // ? 加上这句！引入 SD 卡模块

static const char *TAG = "J.A.R.V.I.S";

// ==========================================
// ?? 您的专属配置 ??
// ==========================================
const char* websocket_url = "ws://124.220.224.189:8765/";

// ==========================================
// ? 全局状态与句柄
// ==========================================
esp_websocket_client_handle_t ws_client;
bool isPlaying = false;
TickType_t lastPlayTime = 0;

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
                isPlaying = true;
                lastPlayTime = xTaskGetTickCount(); // 记录当前 FreeRTOS 滴答时间
                
                // 将收到的音频块直接丢给喇叭播放！
                playSpeaker((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;
    }
}

// ==========================================
// ? 麦克风采集与发送任务 (替代原先的 loop)
// ==========================================
void audio_tx_task(void *pvParameters) {
    const size_t samples = 512; // 每次读取 512 个采样点 (1024 字节)
    int16_t audioBuffer[samples];

    while (1) {
        // 判断是否播放完毕：如果当前时间减去上次收到音频的时间超过了 500ms
        if (isPlaying && ((xTaskGetTickCount() - lastPlayTime) * portTICK_PERIOD_MS > 500)) {
            isPlaying = false;
        }

        // 如果 WebSocket 连着
        if (esp_websocket_client_is_connected(ws_client)) {
            if (!isPlaying) {
                // 没在播放语音，正常采音并发送
                size_t bytesRead = readAudio(audioBuffer, samples);
                if (bytesRead > 0) {
                    esp_websocket_client_send_bin(ws_client, (const char*)audioBuffer, bytesRead, portMAX_DELAY);
                }
            } else {
                // 正在播放语音，读取但不发送，防止“对讲机回音”
                readAudio(audioBuffer, samples); 
            }
        } else {
            // 如果没连上基站，稍微休息一下，防止空跑占用 CPU
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}

void app_main(void) {

    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "System booting...");

    
    // ==========================================

    // 2. 初始化网络与三大硬件
    wifi_init_sta();
    
    ESP_LOGI(TAG, "? 等待 WiFi 分配 IP...");
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    initCamera();
    initAudio();
    initSpeaker();

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

    // 4. 开启独立线程：无情地抓取麦克风数据发给基站
    xTaskCreate(audio_tx_task, "audio_tx_task", 8192, NULL, 5, NULL);

    // 主线程可在此挂起
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}
