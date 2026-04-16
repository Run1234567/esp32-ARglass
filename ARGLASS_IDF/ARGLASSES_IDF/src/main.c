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
#include "app_mqtt.h" // ? 加上这句！引入 MQTT 模块
#include "record_app.h" // ? 加上这句！引入录音模块
static const char *TAG = "J.A.R.V.I.S";

// ==========================================
// ?? 您的专属配置 ??
// ==========================================
const char* websocket_url = "ws://124.220.224.189:8765/";

// ==========================================
// ? 全局状态与句柄
// ==========================================
esp_websocket_client_handle_t ws_client;

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
                playSpeaker((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;
    }
}

// ==========================================
// 🎙️ 麦克风采集与发送任务 (全双工狂飙版)
// ==========================================
void audio_tx_task(void *pvParameters) {
    const size_t samples = 512; // 每次读取 512 个采样点 (1024 字节)
    int16_t audioBuffer[samples];

    while (1) {
        // 如果 WebSocket 连着
        if (esp_websocket_client_is_connected(ws_client)) {
            
            // 无脑读，无脑发！完全不管喇叭是不是在响！
            size_t bytesRead = readAudio(audioBuffer, samples);
            if (bytesRead > 0) {
                esp_websocket_client_send_bin(ws_client, (const char*)audioBuffer, bytesRead, portMAX_DELAY);
            }
            
        } else {
            // 如果没连上基站，稍微休息一下，防止空跑占用 CPU
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}
// ==========================================
// 🚀 独立任务：小说读取与发送专员
// ==========================================
void novel_read_task(void *pvParameters) {
    ESP_LOGI("NOVEL_TASK", "小说读取子任务已启动，正在待命...");

    while (1) {
        // 🚦 核心：在这里等信号，不干活时完全不占 CPU
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

void app_main(void) {
     vTaskDelay(pdMS_TO_TICKS(3000)); 
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 调用模块暴露的接口
    if (init_sd_card() == ESP_OK) {
        test_sd_card_read_write();
    } else {
        ESP_LOGE(TAG, "⚠️ SD 卡模块异常，跳过后续依赖任务...");
    }
    // ==========================================

    // 2. 初始化网络与三大硬件
    wifi_init_sta();
    
    ESP_LOGI(TAG, "? 等待 WiFi 分配 IP...");
    vTaskDelay(pdMS_TO_TICKS(5000)); 

    initCamera();
    initAudio();
    initSpeaker();
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
    app_mqtt_start();

    // 4. 开启独立线程：无情地抓取麦克风数据发给基站
    xTaskCreate(audio_tx_task, "audio_tx_task", 8192, NULL, 5, NULL);
    xTaskCreate(novel_read_task, "novel_task", 4096 * 2, NULL, 5, NULL);
    // // 2. 愉快的业务逻辑演示
    // ESP_LOGI(TAG, "准备开始第一段录音...");
    // vTaskDelay(pdMS_TO_TICKS(2000));
    
    // start_record(); // 它会自动变成 REC_001.wav
    // vTaskDelay(pdMS_TO_TICKS(5000)); // 录 5 秒
    // stop_record();

    // ESP_LOGI(TAG, "休息 3 秒钟...");
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // take_photo_and_save(); // 自动保存为 IMG_002.jpg
    // vTaskDelay(pdMS_TO_TICKS(3000)); // 必须给上一个文件一点点收尾时间，顺便休息下

    // ESP_LOGI(TAG, "准备开始第二段录音...");
    // start_record(); // 它会自动变成 REC_002.wav
    // vTaskDelay(pdMS_TO_TICKS(15000)); // 录 3 秒
    // stop_record();
    // 主线程可在此挂起
    while(1) {
        app_mqtt_publish("home/status/sensor", "TEMP:52");
        vTaskDelay(pdMS_TO_TICKS(10000)); 
    }
}