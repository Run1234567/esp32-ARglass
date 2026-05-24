#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "my_uart.h"
#include "sd_card_app.h"
#include "record_app.h" // ✨ 引入录音控制模块头文件
#include "music_app.h"  // ✨ 引入音乐播放模块头文件

static const char *TAG = "MY_UART";

#define BUF_SIZE (4096)
#define RD_BUF_SIZE (BUF_SIZE)
static QueueHandle_t uart_queue;

// ✨ 声明外部全局开关：记录当前是否允许语音播报
extern uint8_t global_tts_enabled;

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    for(;;) {
        // 等待串口事件 (Waiting for UART event)
        if(xQueueReceive(uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            switch(event.type) {
                case UART_DATA:
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    ESP_LOGI(TAG, "[UART DATA]: %s", dtmp);
                    
                    // ========================================== 
                    // 🧠 数据路由器：通过前缀分类处理数据
                    // ========================================== 
                    
                    // 1️⃣ 判断是不是小说内容数据 (前缀是 "NOV:") 
                    if (strncmp((char*)dtmp, "NOV:", 4) == 0) { 
                        // 这里可以添加将数据注入书库的代码
                    } 
                    
                    // 2️⃣ ✨ 判断是不是 UI 发来的“催更”请求！
                    else if (strncmp((char*)dtmp, "CMD:NOVEL_END", 13) == 0) { 
                        ESP_LOGI(TAG, "📖 收到 UI 催更请求：当前页读完了，立刻安排下一页！"); 
                        test_read_novel_next_chunk(); 
                    } 
                    
                    // ========================================== 
                    // ✨ 处理语音开关指令
                    // ========================================== 
                    else if (strncmp((char*)dtmp, "CMD:TTS_ON", 10) == 0) { 
                        global_tts_enabled = 1; 
                        ESP_LOGI(TAG, "🔊 收到指令：已开启语音播报"); 
                    } 
                    else if (strncmp((char*)dtmp, "CMD:TTS_OFF", 11) == 0) { 
                        global_tts_enabled = 0; 
                        ESP_LOGI(TAG, "🔇 收到指令：已关闭语音播报"); 
                    } 

                    // ========================================== 
                    // ✨ 录音控制协议 
                    // ========================================== 
                    else if (strncmp((char*)dtmp, "CMD:REC_START", 13) == 0 ) { 
                        ESP_LOGI(TAG, "📡 收到 UI 指令：开始录音！" ); 
                        start_record(); 
                    } 
                    else if (strncmp((char*)dtmp, "CMD:REC_STOP", 12) == 0 ) { 
                        ESP_LOGI(TAG, "📡 收到 UI 指令：停止录音！" ); 
                        stop_record();  
                    } 

                    // ========================================== 
                    // 🚀 录音回放控制协议 (加强版，防错位与回车)
                    // ========================================== 
                    // 用 strstr 替代 strncmp，只要字符串里包含这个指令就算数！
                    else if (strstr((char*)dtmp, "CMD:GET_REC_LIST") != NULL) {
                        ESP_LOGI(TAG, "📡 收到 UI 指令：请求录音列表");
                        extern void scan_and_send_record_list(void);
                        scan_and_send_record_list();
                    }
                    else if (strstr((char*)dtmp, "CMD:PLAY_REC:") != NULL) {
                        // 提取冒号后面的文件名
                        char *cmd_pos = strstr((char*)dtmp, "CMD:PLAY_REC:");
                        char *filename = cmd_pos + 13; 
                        
                        // 🧹 极其重要：清理掉尾巴上可能跟着的 \r 或 \n，否则找不到文件
                        for(int i = 0; i < strlen(filename); i++) {
                            if(filename[i] == '\r' || filename[i] == '\n') {
                                filename[i] = '\0';
                                break;
                            }
                        }
                        
                        // 拼接成绝对路径 /sdcard/ly/REC_001.wav
                        char full_path[128];
                        snprintf(full_path, sizeof(full_path), "%s/ly/%s", MOUNT_POINT, filename); 
                        
                        ESP_LOGI(TAG, "▶️ 收到 UI 指令：准备播放 %s", full_path);
                        extern void start_music_player(const char *path);
                        start_music_player(full_path);
                    }

                    // 3️⃣ 预留电池电量数据 (前缀是 "BAT:") 
                    else if (strncmp((char*)dtmp, "BAT:", 4) == 0) { 
                        // ... 
                    }

                    // ========================================== 
                    // 🎵 音乐播放控制协议 
                    // ========================================== 
                    else if (strstr((char*)dtmp, "CMD:PAUSE_MUSIC") != NULL) {
                        extern void pause_music_player(void); pause_music_player();
                    }
                    else if (strstr((char*)dtmp, "CMD:RESUME_MUSIC") != NULL) {
                        extern void resume_music_player(void); resume_music_player();
                    }
                    else if (strstr((char*)dtmp, "CMD:SEEK_MUSIC:") != NULL) {
                        char *val_str = strstr((char*)dtmp, "CMD:SEEK_MUSIC:") + 15;
                        extern void seek_music_player(int sec);
                        seek_music_player(atoi(val_str)); // 字符串转整数
                    }
                    else if (strstr((char*)dtmp, "CMD:STOP_MUSIC") != NULL) {
                        extern void stop_music_player(void); stop_music_player();
                    }

                    // ==========================================
                    // 📸 拍照控制协议
                    // ==========================================
                    else if (strstr((char*)dtmp, "CMD:TAKE_PHOTO") != NULL) {
                        ESP_LOGI(TAG, "📸 收到 UI 指令：执行高清拍照！");
                        extern void execute_high_res_capture(void);
                        execute_high_res_capture();
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "hw fifo overflow");
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "ring buffer full");
                    uart_flush_input(UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                default:
                    break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void my_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 1000000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // 安装串口驱动并获取队列
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 创建串口事件处理守护任务
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, NULL);
    
    ESP_LOGI(TAG, "UART initialized on TX:%d, RX:%d", TXD_PIN, RXD_PIN);
}

void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_NUM, data, strlen(data));
    uart_write_bytes(UART_NUM, "\n", 1);
}