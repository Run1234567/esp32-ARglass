#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "my_uart.h"
#include "sd_card_app.h"
#include "record_app.h"
#include "music_app.h"

static const char *TAG = "MY_UART";

#define RD_BUF_SIZE (4096)
static QueueHandle_t uart_queue;

extern uint8_t global_tts_enabled;

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE + 1);
    for(;;) {
        if(xQueueReceive(uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE + 1);
            switch(event.type) {
                case UART_DATA:
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    dtmp[event.size] = '\0';

                    char *cmd_line = strtok((char*)dtmp, "\r\n");

                    while (cmd_line != NULL) {

                        // ==========================================
                        // 小说核心协议
                        // ==========================================
                        if (strncmp(cmd_line, "NOV:", 4) == 0) {
                        }
                        else if (strncmp(cmd_line, "CMD:NOVEL_END", 13) == 0) {
                            ESP_LOGI(TAG, "收到 UI 催更请求：立刻安排下一页！");
                            extern SemaphoreHandle_t next_page_sem;
                            if (next_page_sem != NULL) {
                                xSemaphoreGive(next_page_sem);
                            }
                        }
                        else if (strncmp(cmd_line, "CMD:MODE:TTS", 12) == 0) {
                            extern uint8_t global_tts_enabled;
                            global_tts_enabled = 1;
                            extern volatile bool is_reading_active;
                            is_reading_active = true;
                            ESP_LOGI(TAG, "模式切换：语音同步");
                        }
                        else if (strncmp(cmd_line, "CMD:MODE:TEXT", 13) == 0) {
                            extern uint8_t global_tts_enabled;
                            global_tts_enabled = 0;
                            extern void stop_tts_reading(void);
                            stop_tts_reading();
                            ESP_LOGI(TAG, "模式切换：纯文本");
                        }
                        else if (strncmp(cmd_line, "CMD:TTS_SPEED:", 14) == 0) {
                            int speed = atoi(cmd_line + 14);
                            extern void tts_set_speed(int speed);
                            tts_set_speed(speed);
                        }
                        else if (strncmp(cmd_line, "CMD:STOP_READING", 16) == 0) {
                            extern void stop_tts_reading(void);
                            stop_tts_reading();
                        }
                        else if (strncmp(cmd_line, "CMD:GET_BOOKS", 13) == 0) {
                            int offset = 0;
                            char *colon = strchr(cmd_line, ':');
                            if (colon) offset = atoi(colon + 1);
                            scan_and_send_book_list(offset);
                        }
                        else if (strncmp(cmd_line, "CMD:GET_CHAPS:", 14) == 0) {
                            char *param = cmd_line + 14;
                            char *comma = strchr(param, ',');
                            int offset = 0;
                            if (comma) {
                                *comma = '\0';
                                offset = atoi(comma + 1);
                            }
                            scan_and_send_chapter_list(param, offset);
                        }
                        else if (strncmp(cmd_line, "CMD:READ_CHAP:", 14) == 0) {
                            char *rel_path = cmd_line + 14;

                            extern SemaphoreHandle_t next_page_sem;
                            snprintf(current_novel_path, sizeof(current_novel_path), "%s/小说/%s", MOUNT_POINT, rel_path);
                            current_file_offset = 0;

                            ESP_LOGI(TAG, "准备阅读: %s", current_novel_path);

                            extern volatile bool is_reading_active;
                            is_reading_active = true;
                            if (next_page_sem != NULL) xSemaphoreGive(next_page_sem);
                        }

                        // ==========================================
                        // 录音控制协议
                        // ==========================================
                        else if (strncmp(cmd_line, "CMD:REC_START", 13) == 0) {
                            start_record();
                        }
                        else if (strncmp(cmd_line, "CMD:REC_STOP", 12) == 0) {
                            stop_record();
                        }
                        else if (strstr(cmd_line, "CMD:GET_REC_LIST") != NULL) {
                            extern void scan_and_send_record_list(void);
                            scan_and_send_record_list();
                        }
                        else if (strstr(cmd_line, "CMD:PLAY_REC:") != NULL) {
                            char *filename = strstr(cmd_line, "CMD:PLAY_REC:") + 13;
                            char full_path[128];
                            snprintf(full_path, sizeof(full_path), "%s/录音/%s", MOUNT_POINT, filename);
                            extern void start_music_player(const char *path);
                            start_music_player(full_path);
                        }

                        // ==========================================
                        // 音乐播放控制协议
                        // ==========================================
                        else if (strstr(cmd_line, "CMD:PAUSE_MUSIC")) {
                            extern void pause_music_player(void); pause_music_player();
                        }
                        else if (strstr(cmd_line, "CMD:RESUME_MUSIC")) {
                            extern void resume_music_player(void); resume_music_player();
                        }
                        else if (strstr(cmd_line, "CMD:SEEK_MUSIC:")) {
                            int sec = atoi(strstr(cmd_line, "CMD:SEEK_MUSIC:") + 15);
                            extern void seek_music_player(int sec); seek_music_player(sec);
                        }
                        else if (strstr(cmd_line, "CMD:STOP_MUSIC")) {
                            extern void stop_music_player(void); stop_music_player();
                        }
                        else if (strstr(cmd_line, "CMD:GET_MUSIC_LIST")) {
                            extern void scan_and_send_music_list(void); scan_and_send_music_list();
                        }
                        else if (strstr(cmd_line, "CMD:PLAY_YY:")) {
                            char *filename = strstr(cmd_line, "CMD:PLAY_YY:") + 12;
                            char full_path[128];
                            snprintf(full_path, sizeof(full_path), "%s/音乐/%s", MOUNT_POINT, filename);
                            extern void send_lrc_to_ui(const char* song_name);
                            send_lrc_to_ui(filename);
                            extern void start_music_player(const char *path);
                            start_music_player(full_path);
                        }
                        else if (strstr(cmd_line, "CMD:VOL:")) {
                            int vol = atoi(strstr(cmd_line, "CMD:VOL:") + 8);
                            extern void set_music_volume(int vol); set_music_volume(vol);
                        }

                        // ==========================================
                        // 拍照与其他硬件协议
                        // ==========================================
                        else if (strstr(cmd_line, "CMD:TAKE_PHOTO")) {
                            extern void execute_high_res_capture(void); execute_high_res_capture();
                        }
                        else if (strstr(cmd_line, "CMD:NOISE_ON")) {
                            extern volatile bool send_noise_data; send_noise_data = true;
                        }
                        else if (strstr(cmd_line, "CMD:NOISE_OFF")) {
                            extern volatile bool send_noise_data; send_noise_data = false;
                        }
                        else if (strstr(cmd_line, "CMD:PITCH_ON")) {
                            extern void start_yin_pitch_task(void); start_yin_pitch_task();
                        }
                        else if (strstr(cmd_line, "CMD:PITCH_OFF")) {
                            extern void stop_yin_pitch_task(void); stop_yin_pitch_task();
                        }

                        cmd_line = strtok(NULL, "\r\n");
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

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, NULL);

    ESP_LOGI(TAG, "UART initialized on TX:%d, RX:%d", TXD_PIN, RXD_PIN);
}

void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_NUM, data, strlen(data));
}
