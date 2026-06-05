/**
 * @file my_uart.c
 * @brief UART 命令接口模块 - 与外部 UI MCU 通信
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 通过 UART1 以 1Mbaud 高速串口与外部 UI 微控制器通信。
 * UI MCU 负责驱动 AR 眼镜的显示屏幕和按键输入。
 *
 * 硬件连接：
 *   - GPIO 4: TX (发送) - ESP32 -> UI MCU
 *   - GPIO 5: RX (接收) - UI MCU -> ESP32
 *   - 波特率: 1,000,000 (1Mbaud)
 *
 * 命令协议 (UI MCU -> ESP32):
 * =====================================================================
 * 【小说阅读命令】
 *   CMD:NOVEL_END           - UI 请求下一页 (当前页已显示完毕)
 *   CMD:MODE:TTS            - 切换到 TTS 语音同步模式
 *   CMD:MODE:TEXT           - 切换到纯文本显示模式
 *   CMD:TTS_SPEED:N         - 设置 TTS 语速 (N=0~9)
 *   CMD:STOP_READING        - 停止小说阅读
 *   CMD:GET_BOOKS[:offset]  - 获取书籍列表 (支持分页)
 *   CMD:GET_CHAPS:书名,偏移 - 获取章节列表
 *   CMD:READ_CHAP:路径      - 开始阅读指定章节
 *
 * 【录音控制命令】
 *   CMD:REC_START           - 开始录音
 *   CMD:REC_STOP            - 停止录音
 *   CMD:GET_REC_LIST        - 获取录音文件列表
 *   CMD:PLAY_REC:文件名     - 播放指定录音文件
 *
 * 【音乐播放命令】
 *   CMD:PAUSE_MUSIC         - 暂停播放
 *   CMD:RESUME_MUSIC        - 继续播放
 *   CMD:SEEK_MUSIC:秒数     - 跳转到指定时间
 *   CMD:STOP_MUSIC          - 停止播放
 *   CMD:GET_MUSIC_LIST      - 获取音乐文件列表
 *   CMD:PLAY_YY:文件名      - 播放指定音乐文件 (同时加载歌词)
 *   CMD:VOL:N               - 设置音量 (N=0~100)
 *
 * 【拍照命令】
 *   CMD:TAKE_PHOTO          - 拍照并保存
 *
 * 【音频分析命令】
 *   CMD:NOISE_ON            - 开启分贝监测
 *   CMD:NOISE_OFF           - 关闭分贝监测
 *   CMD:PITCH_ON            - 开启音调检测
 *   CMD:PITCH_OFF           - 关闭音调检测
 *
 * 数据上报 (ESP32 -> UI MCU):
 *   NOV:文本                - 小说文本内容
 *   BK:书名 / CH:章节名     - 书籍/章节列表
 *   MU:文件名               - 音乐列表
 *   LRC:秒数:歌词           - 歌词数据
 *   REC_FILE:文件名         - 录音列表
 *   DB:分贝值               - 噪声数据
 *   PH:频率                 - 音调数据
 *   AUDIO_INFO:CUR:秒 / TOT:秒 - 播放进度/总时长
 *   CMD:PHOTO_DONE          - 拍照完成通知
 *
 * 依赖组件：
 *   - driver:      UART 驱动
 *   - sd_card_app: 书籍/章节/音乐列表扫描
 *   - record_app:  录音控制
 *   - music_app:   音乐播放控制
 */

#include <stdio.h>
#include <string.h>

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"     // UART 事件队列
#include "freertos/semphr.h"    // 信号量

/* ==================== ESP-IDF 驱动头文件 ==================== */
#include "driver/uart.h"        // UART 驱动
#include "driver/gpio.h"        // GPIO 驱动
#include "esp_log.h"            // 日志

/* ==================== 项目组件头文件 ==================== */
#include "my_uart.h"            // 自身头文件
#include "sd_card_app.h"        // SD 卡模块 (书籍/音乐扫描)
#include "record_app.h"         // 录音模块
#include "music_app.h"          // 音乐播放模块

static const char *TAG = "MY_UART";  // 日志标签

/* =====================================================================
 * UART 接收缓冲区配置
 * ===================================================================== */
#define RD_BUF_SIZE (4096)  // 接收缓冲区大小: 4KB

/**
 * @brief UART 事件队列
 * UART 驱动将接收到的事件 (数据到达/缓冲区满等) 放入此队列
 */
static QueueHandle_t uart_queue;

/**
 * @brief 外部引用：TTS 模式开关
 * 由 CMD:MODE:TTS/TEXT 命令切换
 */
extern uint8_t global_tts_enabled;

/* =====================================================================
 * UART 事件处理任务
 * =====================================================================
 * @brief 运行在独立任务中的 UART 命令解析器
 *
 * 工作流程：
 *   1. 从 uart_queue 阻塞等待 UART 事件
 *   2. 收到 UART_DATA 事件时，读取数据并按 \r\n 分割命令行
 *   3. 逐行匹配命令前缀，分发到对应的处理函数
 *
 * 优先级: 12 (非常高！UI 响应需要低延迟)
 * 栈大小: 4096 字节
 */
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE + 1);  // 分配接收缓冲区

    for(;;) {
        // 从 UART 事件队列接收事件 (无限等待)
        if(xQueueReceive(uart_queue, (void * )&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, RD_BUF_SIZE + 1);  // 清空缓冲区

            switch(event.type) {
                case UART_DATA:
                    /* ---- 收到数据 ---- */
                    uart_read_bytes(UART_NUM, dtmp, event.size, portMAX_DELAY);
                    dtmp[event.size] = '\0';  // 添加字符串结束符

                    // 按 \r\n 分割多条命令 (UI 可能一次发送多条)
                    char *cmd_line = strtok((char*)dtmp, "\r\n");

                    while (cmd_line != NULL) {

                        /* =================================================
                         * 小说阅读协议
                         * ================================================= */

                        // NOV: - UI 返回的小说确认 (无需处理)
                        if (strncmp(cmd_line, "NOV:", 4) == 0) {
                            // 预留：UI 端的确认消息
                        }

                        // CMD:NOVEL_END - UI 请求下一页
                        else if (strncmp(cmd_line, "CMD:NOVEL_END", 13) == 0) {
                            ESP_LOGI(TAG, "收到 UI 催更请求：立刻安排下一页！");
                            extern SemaphoreHandle_t next_page_sem;
                            if (next_page_sem != NULL) {
                                xSemaphoreGive(next_page_sem);  // 唤醒小说读取任务
                            }
                        }

                        // CMD:MODE:TTS - 切换到 TTS 语音同步模式
                        else if (strncmp(cmd_line, "CMD:MODE:TTS", 12) == 0) {
                            extern uint8_t global_tts_enabled;
                            global_tts_enabled = 1;  // 开启 TTS
                            extern volatile bool is_reading_active;
                            is_reading_active = true;  // 进入小说阅读模式
                            ESP_LOGI(TAG, "模式切换：语音同步");
                        }

                        // CMD:MODE:TEXT - 切换到纯文本显示模式
                        else if (strncmp(cmd_line, "CMD:MODE:TEXT", 13) == 0) {
                            extern uint8_t global_tts_enabled;
                            global_tts_enabled = 0;  // 关闭 TTS
                            extern void stop_tts_reading(void);
                            stop_tts_reading();       // 停止当前 TTS 播放
                            ESP_LOGI(TAG, "模式切换：纯文本");
                        }

                        // CMD:TTS_SPEED:N - 设置 TTS 语速
                        else if (strncmp(cmd_line, "CMD:TTS_SPEED:", 14) == 0) {
                            int speed = atoi(cmd_line + 14);  // 提取速度值
                            extern void tts_set_speed(int speed);
                            tts_set_speed(speed);
                        }

                        // CMD:STOP_READING - 停止小说阅读
                        else if (strncmp(cmd_line, "CMD:STOP_READING", 16) == 0) {
                            extern void stop_tts_reading(void);
                            stop_tts_reading();
                        }

                        // CMD:GET_BOOKS[:offset] - 获取书籍列表
                        else if (strncmp(cmd_line, "CMD:GET_BOOKS", 13) == 0) {
                            int offset = 0;
                            char *colon = strchr(cmd_line, ':');
                            if (colon) offset = atoi(colon + 1);  // 提取分页偏移
                            scan_and_send_book_list(offset);
                        }

                        // CMD:GET_CHAPS:书名,偏移 - 获取章节列表
                        else if (strncmp(cmd_line, "CMD:GET_CHAPS:", 14) == 0) {
                            char *param = cmd_line + 14;
                            char *comma = strchr(param, ',');
                            int offset = 0;
                            if (comma) {
                                *comma = '\0';           // 分割书名和偏移
                                offset = atoi(comma + 1);
                            }
                            scan_and_send_chapter_list(param, offset);
                        }

                        // CMD:READ_CHAP:相对路径 - 开始阅读指定章节
                        else if (strncmp(cmd_line, "CMD:READ_CHAP:", 14) == 0) {
                            char *rel_path = cmd_line + 14;

                            extern SemaphoreHandle_t next_page_sem;
                            // 构建完整路径并设置全局变量
                            snprintf(current_novel_path, sizeof(current_novel_path),
                                     "%s/小说/%s", MOUNT_POINT, rel_path);
                            current_file_offset = 0;  // 重置书签到文件开头

                            ESP_LOGI(TAG, "准备阅读: %s", current_novel_path);

                            // 激活小说阅读模式并触发第一次读取
                            extern volatile bool is_reading_active;
                            is_reading_active = true;
                            if (next_page_sem != NULL) xSemaphoreGive(next_page_sem);
                        }

                        /* =================================================
                         * 录音控制协议
                         * ================================================= */

                        // CMD:REC_START - 开始录音
                        else if (strncmp(cmd_line, "CMD:REC_START", 13) == 0) {
                            start_record();
                        }

                        // CMD:REC_STOP - 停止录音
                        else if (strncmp(cmd_line, "CMD:REC_STOP", 12) == 0) {
                            stop_record();
                        }

                        // CMD:GET_REC_LIST - 获取录音文件列表
                        else if (strstr(cmd_line, "CMD:GET_REC_LIST") != NULL) {
                            extern void scan_and_send_record_list(void);
                            scan_and_send_record_list();
                        }

                        // CMD:PLAY_REC:文件名 - 播放指定录音
                        else if (strstr(cmd_line, "CMD:PLAY_REC:") != NULL) {
                            char *filename = strstr(cmd_line, "CMD:PLAY_REC:") + 13;
                            char full_path[128];
                            snprintf(full_path, sizeof(full_path), "%s/录音/%s", MOUNT_POINT, filename);
                            extern void start_music_player(const char *path);
                            start_music_player(full_path);  // 复用音乐播放器
                        }

                        /* =================================================
                         * 音乐播放控制协议
                         * ================================================= */

                        // CMD:PAUSE_MUSIC - 暂停播放
                        else if (strstr(cmd_line, "CMD:PAUSE_MUSIC")) {
                            extern void pause_music_player(void); pause_music_player();
                        }

                        // CMD:RESUME_MUSIC - 继续播放
                        else if (strstr(cmd_line, "CMD:RESUME_MUSIC")) {
                            extern void resume_music_player(void); resume_music_player();
                        }

                        // CMD:SEEK_MUSIC:秒数 - 跳转播放进度
                        else if (strstr(cmd_line, "CMD:SEEK_MUSIC:")) {
                            int sec = atoi(strstr(cmd_line, "CMD:SEEK_MUSIC:") + 15);
                            extern void seek_music_player(int sec); seek_music_player(sec);
                        }

                        // CMD:STOP_MUSIC - 停止播放
                        else if (strstr(cmd_line, "CMD:STOP_MUSIC")) {
                            extern void stop_music_player(void); stop_music_player();
                        }

                        // CMD:GET_MUSIC_LIST - 获取音乐列表
                        else if (strstr(cmd_line, "CMD:GET_MUSIC_LIST")) {
                            extern void scan_and_send_music_list(void); scan_and_send_music_list();
                        }

                        // CMD:PLAY_YY:文件名 - 播放音乐 (同时加载歌词)
                        else if (strstr(cmd_line, "CMD:PLAY_YY:")) {
                            char *filename = strstr(cmd_line, "CMD:PLAY_YY:") + 12;
                            char full_path[128];
                            snprintf(full_path, sizeof(full_path), "%s/音乐/%s", MOUNT_POINT, filename);
                            // 先加载歌词
                            extern void send_lrc_to_ui(const char* song_name);
                            send_lrc_to_ui(filename);
                            // 再开始播放
                            extern void start_music_player(const char *path);
                            start_music_player(full_path);
                        }

                        // CMD:VOL:N - 设置音量
                        else if (strstr(cmd_line, "CMD:VOL:")) {
                            int vol = atoi(strstr(cmd_line, "CMD:VOL:") + 8);
                            extern void set_music_volume(int vol); set_music_volume(vol);
                        }

                        /* =================================================
                         * 拍照与其他硬件协议
                         * ================================================= */

                        // CMD:TAKE_PHOTO - 拍照
                        else if (strstr(cmd_line, "CMD:TAKE_PHOTO")) {
                            extern void execute_high_res_capture(void); execute_high_res_capture();
                        }

                        // CMD:NOISE_ON/OFF - 分贝监测开关
                        else if (strstr(cmd_line, "CMD:NOISE_ON")) {
                            extern volatile bool send_noise_data; send_noise_data = true;
                        }
                        else if (strstr(cmd_line, "CMD:NOISE_OFF")) {
                            extern volatile bool send_noise_data; send_noise_data = false;
                        }

                        // CMD:PITCH_ON/OFF - 音调检测开关
                        else if (strstr(cmd_line, "CMD:PITCH_ON")) {
                            extern void start_yin_pitch_task(void); start_yin_pitch_task();
                        }
                        else if (strstr(cmd_line, "CMD:PITCH_OFF")) {
                            extern void stop_yin_pitch_task(void); stop_yin_pitch_task();
                        }

                        // 继续解析下一条命令
                        cmd_line = strtok(NULL, "\r\n");
                    }
                    break;

                case UART_FIFO_OVF:
                    /* ---- 硬件 FIFO 溢出 ---- */
                    ESP_LOGI(TAG, "hw fifo overflow");
                    uart_flush_input(UART_NUM);   // 清空输入缓冲
                    xQueueReset(uart_queue);       // 重置事件队列
                    break;

                case UART_BUFFER_FULL:
                    /* ---- 软件缓冲区满 ---- */
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

/* =====================================================================
 * UART 初始化函数
 * =====================================================================
 * @brief 初始化 UART1 驱动，配置 1Mbaud 波特率，启动事件处理任务
 *
 * UART 参数：
 *   - 波特率: 1,000,000 (1Mbaud，高速通信)
 *   - 数据位: 8
 *   - 校验位: 无
 *   - 停止位: 1
 *   - 流控: 无
 *   - TX: GPIO 4
 *   - RX: GPIO 5
 */
void my_uart_init(void) {
    /* ---- UART 配置 ---- */
    uart_config_t uart_config = {
        .baud_rate = 1000000,                    // 1Mbaud 波特率
        .data_bits = UART_DATA_8_BITS,           // 8 数据位
        .parity = UART_PARITY_DISABLE,           // 无校验
        .stop_bits = UART_STOP_BITS_1,           // 1 停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,   // 无硬件流控
        .source_clk = UART_SCLK_DEFAULT,         // 默认时钟源
    };

    /* ---- 安装 UART 驱动 ---- */
    // 参数: UART号, TX缓冲区, RX缓冲区, 事件队列大小, 队列句柄, 中断标志
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    /* ---- 启动事件处理任务 ---- */
    // 优先级 12：非常高，确保 UI 命令能被及时处理
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, NULL);

    ESP_LOGI(TAG, "UART initialized on TX:%d, RX:%d", TXD_PIN, RXD_PIN);
}

/* =====================================================================
 * UART 数据发送函数
 * =====================================================================
 * @brief 通过 UART1 发送字符串数据给 UI MCU
 *
 * @param data 要发送的字符串 (以 '\0' 结尾)
 */
void my_uart_send(const char* data) {
    if (data == NULL) return;
    uart_write_bytes(UART_NUM, data, strlen(data));
}
