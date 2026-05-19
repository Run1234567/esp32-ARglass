#include "my_uart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ui_globals.h" // 引入全局变量以操作小说缓冲区

#define UART_PORT_NUM      UART_NUM_1       // 使用 UART1
#define UART_BAUD_RATE     115200           // 波特率
#define UART_TXD_PIN       45               // TX 引脚
#define UART_RXD_PIN       46               // RX 引脚
#define BUF_SIZE           2048             // 缓冲区调大到 2KB

static const char *TAG = "MY_UART";
static QueueHandle_t uart_queue;

// ==========================================
// ? 串口发送函数
// ==========================================
void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_PORT_NUM, data, strlen(data));
}

// ==========================================
// ? 串口接收事件任务
// ==========================================
static void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE);
    
    while (1) {
        if (xQueueReceive(uart_queue, (void * )&event, portMAX_DELAY)) {
            bzero(dtmp, BUF_SIZE);
            
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(UART_PORT_NUM, dtmp, event.size, portMAX_DELAY);
                if (len > 0) {
                    dtmp[len] = '\0'; // 封口为字符串
                    
                    // ? 前缀过滤：只有以 "NOV:" 开头的数据才会被当作小说处理！
                    if (strncmp((char*)dtmp, "NOV:", 4) == 0) {
                        char *payload = (char*)dtmp + 4; // 跳过 "NOV:" 这4个字符
                        
                        // 释放旧书库内存
                        if (novel_source_buffer != NULL) {
                            free(novel_source_buffer);
                            novel_source_buffer = NULL;
                        }
                        
                        // 分配新书库内存并注入
                        novel_source_buffer = (char*) malloc(strlen(payload) + 1);
                        if (novel_source_buffer != NULL) {
                            strcpy(novel_source_buffer, payload);
                            current_book_pos = 0;              // 重置书签
                            novel_scroll_task_running = 0;     // 允许继续滚动
                            ESP_LOGI(TAG, "? 小说新章节注入成功，长度: %d", strlen(payload));
                        }
                    } else {
                        ESP_LOGI(TAG, "? 串口收到其他数据: %s", dtmp);
                    }
                }
            }
            // 处理溢出异常
            else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                uart_flush_input(UART_PORT_NUM);
                xQueueReset(uart_queue);
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}

// ==========================================
// ? 初始化函数
// ==========================================
void my_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD_PIN, UART_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));

    xTaskCreate(uart_event_task, "uart_event", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "? 串口通信模块初始化完毕！(TX:45, RX:46)");
}
