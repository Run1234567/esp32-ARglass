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
#include "esp_heap_caps.h"
#include "ui_globals.h"

#define UART_PORT_NUM      UART_NUM_1
#define UART_BAUD_RATE     1000000
#define UART_TXD_PIN       45
#define UART_RXD_PIN       46
#define BUF_SIZE           4096

static const char *TAG = "MY_UART";
static QueueHandle_t uart_queue;

void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_PORT_NUM, data, strlen(data));
}

static void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(BUF_SIZE + 1);

    while (1) {
        if (xQueueReceive(uart_queue, (void * )&event, portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(UART_PORT_NUM, dtmp, event.size, portMAX_DELAY);
                if (len > 0) {
                    dtmp[len] = '\0';

                    if (strncmp((char*)dtmp, "NOV:", 4) == 0) {
                        char *payload = (char*)dtmp + 4;
                        if (novel_source_buffer != NULL) {
                            free(novel_source_buffer);
                            novel_source_buffer = NULL;
                        }
                        novel_source_buffer = (char*) malloc(strlen(payload) + 1);
                        if (novel_source_buffer != NULL) {
                            strcpy(novel_source_buffer, payload);
                            current_book_pos = 0;
                            novel_scroll_task_running = 0;
                        }
                    }
                    else if (strncmp((char*)dtmp, "CMD:CLEAR_LIST", 14) == 0) { extern void playlist_clear(void); playlist_clear(); }
                    else if (strncmp((char*)dtmp, "REC_FILE:", 9) == 0) { extern void playlist_add_file(const char* filename); playlist_add_file((char*)dtmp + 9); }
                    else if (strncmp((char*)dtmp, "CMD:LIST_END", 12) == 0) { extern void playlist_update_ui(void); playlist_update_ui(); }
                    else if (strstr((char*)dtmp, "AUDIO_INFO:TOTAL:") != NULL) {
                        extern void playlist_set_total_time(int t_sec);
                        playlist_set_total_time(atoi(strstr((char*)dtmp, "AUDIO_INFO:TOTAL:") + 17));
                    }
                    else if (strncmp((char*)dtmp, "AUDIO_INFO:TOT:", 15) == 0) {
                        int t_sec = atoi((char*)dtmp + 15);
                        
                        extern void playlist_set_total_time(int t_sec);
                        playlist_set_total_time(t_sec);
                        
                        extern void music_set_total_time(int t_sec);
                        music_set_total_time(t_sec);
                    }
                    else if (strncmp((char*)dtmp, "AUDIO_INFO:CUR:", 15) == 0) {
                        int cur_sec = atoi((char*)dtmp + 15);
                        
                        extern void playlist_update_progress(int cur_sec);
                        playlist_update_progress(cur_sec);
                        
                        extern void music_update_progress(int cur_sec);
                        music_update_progress(cur_sec);
                    }
                    else if (strstr((char*)dtmp, "CMD:PHOTO_DONE") != NULL) {
                        extern void camera_reset_status_label(void);
                        camera_reset_status_label();
                    }
                    else if (strncmp((char*)dtmp, "DB:", 3) == 0) {
                        int db_value = atoi((char*)dtmp + 3);
                        extern void update_noise_meter(int val);
                        update_noise_meter(db_value);
                    }
                    else if (strncmp((char*)dtmp, "PH:", 3) == 0) {
                        float freq = atof((char*)dtmp + 3);
                        extern void update_pitch_ui(float freq);
                        update_pitch_ui(freq);
                    }
                    else if (strstr((char*)dtmp, "MU_CLEAR:1") != NULL) {
                        extern void music_clear_playlist(void);
                        music_clear_playlist();
                    }
                    else if (strncmp((char*)dtmp, "MU:", 3) == 0) {
                        char *song_name = (char*)dtmp + 3;
                        for (int i = 0; i < strlen(song_name); i++) {
                            if (song_name[i] == '\r' || song_name[i] == '\n') {
                                song_name[i] = '\0'; break;
                            }
                        }
                        extern void music_add_song(const char* name);
                        music_add_song(song_name);
                    }
                    else if (strstr((char*)dtmp, "MU_END:1") != NULL) {
                        extern void music_apply_playlist(void);
                        music_apply_playlist();
                    }
                    else if (strstr((char*)dtmp, "LRC_CLR") != NULL) {
                        extern void music_clear_lrc(void);
                        music_clear_lrc();
                    }
                    else if (strncmp((char*)dtmp, "LRC:", 4) == 0) {
                        int sec = atoi((char*)dtmp + 4);
                        char *text_start = strchr((char*)dtmp + 4, ':');
                        if (text_start != NULL) {
                            text_start++;
                            text_start[strcspn(text_start, "\r\n")] = '\0';
                            extern void music_add_lrc_line(int sec, const char* text);
                            music_add_lrc_line(sec, text_start);
                        }
                    }

                }
            }
            else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
                uart_flush_input(UART_PORT_NUM);
                xQueueReset(uart_queue);
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}

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
    ESP_LOGI(TAG, "UART init done (TX:45, RX:46)");
}
