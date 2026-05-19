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

static const char *TAG = "MY_UART";

#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
static QueueHandle_t uart_queue;

// ? 引入跨文件全局开关
extern uint8_t global_tts_enabled;

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    for(;;) {
        // Waiting for UART event.
        if(xQueueReceive(uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE);
            switch(event.type) {
                case UART_DATA:
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    ESP_LOGI(TAG, "[UART DATA]: %s", dtmp);
                    
                    // ========================================== 
                    // ? 数据路由器：通过前缀分类处理数据 
                    // ========================================== 
                    
                    // 1?? 判断是不是小说内容数据 (前缀是 "NOV:") 
                    if (strncmp((char*)dtmp, "NOV:", 4) == 0) { 
                        // 这里可以添加将数据注入书库的代码
                    } 
                    
                    // 2?? ? 判断是不是 UI 发来的“催更”请求！ 
                    else if (strncmp((char*)dtmp, "CMD:NOVEL_END", 13) == 0) { 
                        ESP_LOGI(TAG, "? 收到 UI 催更请求：当前页读完了，立刻安排下一页！"); 
                        
                        // 核心魔法：直接呼叫 SD 卡，让它读下一段并发送！ 
                        test_read_novel_next_chunk(); 
                    } 
                    
                    // ========================================== 
                    // ? 新增：处理语音开关指令 
                    // ========================================== 
                    else if (strncmp((char*)dtmp, "CMD:TTS_ON", 10) == 0) { 
                        global_tts_enabled = 1; // 打开开关 
                        ESP_LOGI(TAG, "? 收到指令：已开启语音播报"); 
                    } 
                    else if (strncmp((char*)dtmp, "CMD:TTS_OFF", 11) == 0) { 
                        global_tts_enabled = 0; // 关闭开关 
                        ESP_LOGI(TAG, "? 收到指令：已关闭语音播报"); 
                    } 

                    // 3?? 预留：电池电量数据 (前缀是 "BAT:") 
                    else if (strncmp((char*)dtmp, "BAT:", 4) == 0) { 
                        // ... 
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
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, NULL);
    
    ESP_LOGI(TAG, "UART initialized on TX:%d, RX:%d", TXD_PIN, RXD_PIN);
}

void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_NUM, data, strlen(data));
    uart_write_bytes(UART_NUM, "\n", 1);
}
