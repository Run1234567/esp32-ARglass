/**
 * @file tts_app.c
 * @brief 中文 TTS (Text-to-Speech) 语音合成模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 使用乐鑫 ESP-TTS 引擎将中文文本转换为语音并播放。
 * 采用 "小乐 (Xiaole)" 中文语音模型，支持：
 *   - 中文文本实时合成
 *   - 可变语速控制
 *   - 长文本自动按标点切句
 *   - 异步队列式播放 (不阻塞调用者)
 *   - 强制打断 (stop_tts_reading)
 *   - 小说阅读模式 (自动发信号请求下一页)
 *
 * 工作架构：
 *   调用者 -> tts_speak() -> 切句 -> 队列 -> tts_main_task -> TTS引擎 -> 扬声器
 *
 *   tts_speak(): 接收长文本，按标点符号切成 <=90 字节的短句，放入队列
 *   tts_main_task(): 从队列取出句子，逐个合成 PCM 音频并播放
 *
 * TTS 模型加载：
 *   优先从 SD 卡加载 /sdcard/esp_tts_voice_data_xiaole.dat 到 PSRAM
 *   PSRAM 容量大 (约 8MB)，可以存放 2.9MB 的语音模型
 *
 * 依赖组件：
 *   - esp-sr:    ESP-TTS 引擎 (esp_tts_parse_chinese 等)
 *   - speaker_app: 扬声器播放 (playSpeaker)
 *   - my_uart:     串口通信 (小说模式下发送文本给 UI)
 */

#include "tts_app.h"
#include "esp_log.h"
#include "esp_tts.h"                    // ESP-TTS 引擎核心 API
#include "esp_tts_voice_xiaole.h"       // "小乐" 语音模型定义
#include "speaker_app.h"                // 扬声器播放
#include "esp_heap_caps.h"              // PSRAM 内存分配 (heap_caps_malloc)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"             // 消息队列
#include "my_uart.h"                    // 串口通信
#include <string.h>
#include <ctype.h>                      // ispunct, iscntrl, isspace

static const char *TAG = "TTS_APP";     // 日志标签
#define MOUNT_POINT "/sdcard"           // SD 卡挂载点

/* =====================================================================
 * 全局控制变量
 * ===================================================================== */
volatile int global_tts_speed = 4;        // 语速等级 (默认 4，范围 0-9)
volatile bool is_reading_active = false;  // 是否处于连续小说阅读模式
volatile bool force_stop_tts = false;     // 强制打断标志 (true 时立即停止播放)

/* =====================================================================
 * 内部静态变量
 * ===================================================================== */
static esp_tts_handle_t *tts_handle = NULL;       // TTS 引擎句柄
static uint8_t *model_data_in_psram = NULL;       // PSRAM 中的语音模型数据
static QueueHandle_t tts_queue = NULL;            // 文本消息队列 (char* 指针)

/* =====================================================================
 * 中文标点检测辅助函数
 * =====================================================================
 * @brief 检查字符串开头是否是中文标点符号，返回标点的字节长度
 *
 * ESP-TTS 在遇到标点符号时会产生自然的停顿和语调变化，
 * 因此标点是切句的最佳位置。
 *
 * @param str 要检查的 UTF-8 字符串
 * @return 标点的字节长度 (3 字节的中文标点)，0 表示不是标点
 */
static int skip_zh_punctuation(const char *str) {
    // 常见中文标点列表 (每个占 3 字节 UTF-8)
    const char *zh_puncs[] = {
        "，", "。", "！", "？", "：", "；", "、",    // 句号类
        "“", "”", "‘", "’",       // 引号 (可能显示为 Unicode)
        "（", "）", "【", "】", "《", "》",            // 括号类
        "…", "—", "～",                                // 特殊符号
        NULL  // 结束标记
    };
    for (int i = 0; zh_puncs[i] != NULL; i++) {
        int len = strlen(zh_puncs[i]);
        if (strncmp(str, zh_puncs[i], len) == 0) return len;
    }
    return 0;  // 不是中文标点
}

/* =====================================================================
 * 语速设置
 * =====================================================================
 * @brief 设置 TTS 合成语速
 * @param speed 语速等级 (0=最慢, 4=默认, 9=最快)
 */
void tts_set_speed(int speed) {
    global_tts_speed = speed;
}

/* =====================================================================
 * 强制停止 TTS 播放
 * =====================================================================
 * @brief 立即停止当前 TTS 播放，并清空等待队列
 *
 * 用于：
 *   - 用户退出小说阅读界面
 *   - 切换到纯文本模式
 *   - 需要立即中断语音的情况
 */
void stop_tts_reading(void) {
    is_reading_active = false;   // 停止小说阅读模式
    force_stop_tts = true;       // 设置打断标志

    // 清空队列中所有等待的句子，释放内存
    char *temp;
    while(xQueueReceive(tts_queue, &temp, 0) == pdTRUE) {
        free(temp);  // 每个句子都是 strdup 分配的，需要 free
    }
}

/* =====================================================================
 * 从 SD 卡加载 TTS 语音模型
 * =====================================================================
 * @brief 将 SD 卡上的语音模型文件加载到 PSRAM
 *
 * 模型文件: /sdcard/esp_tts_voice_data_xiaole.dat (约 2.9MB)
 * 加载到 PSRAM 后，TTS 引擎可以快速访问语音数据
 *
 * @param path 模型文件路径
 * @return ESP_OK: 成功; ESP_FAIL: 文件不存在; ESP_ERR_NO_MEM: PSRAM 不足
 */
esp_err_t load_tts_model_from_sd(const char* path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 无法打开模型文件: %s", path);
        return ESP_FAIL;
    }

    // 获取文件大小
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // 在 PSRAM 中分配内存 (MALLOC_CAP_SPIRAM 标志)
    model_data_in_psram = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (model_data_in_psram == NULL) {
        ESP_LOGE(TAG, "❌ PSRAM 内存不足！");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    // 一次性读取整个模型文件
    fread(model_data_in_psram, 1, size, f);
    fclose(f);

    ESP_LOGI(TAG, "📦 TTS 模型已加载到 PSRAM (%zu bytes)", size);
    return ESP_OK;
}

/* =====================================================================
 * TTS 合成与播放任务 (核心任务)
 * =====================================================================
 * @brief 运行在 Core 0 的 TTS 合成任务，从队列读取文本并合成播放
 *
 * 工作流程：
 *   1. 从 tts_queue 队列阻塞等待文本句子
 *   2. 获取扬声器互斥锁 (speaker_mutex)
 *   3. 如果是小说模式，将文本发送给 UI 显示
 *   4. 调用 esp_tts_parse_chinese() 开始合成
 *   5. 循环调用 esp_tts_stream_play() 获取 PCM 数据并播放
 *   6. 如果 force_stop_tts 为 true，立即中断播放
 *   7. 释放互斥锁和文本内存
 *   8. 如果是小说模式且队列为空，释放 next_page_sem 信号请求下一页
 *
 * 优先级: 5
 * 栈大小: 32768 字节 (32KB，TTS 合成需要较大栈空间)
 * 核心绑定: Core 0
 */
void tts_main_task(void *pvParameters) {
    char *current_text = NULL;

    while (1) {
        // 从队列获取下一个待合成的句子 (无限等待)
        if (xQueueReceive(tts_queue, &current_text, portMAX_DELAY) == pdTRUE) {

            // 获取扬声器互斥锁 (防止与其他播放任务冲突)
            xSemaphoreTake(speaker_mutex, portMAX_DELAY);

            force_stop_tts = false;  // 每次拿到新句子，重置打断标志

            /* ---- 小说模式：将文本发送给 UI 屏幕显示 ---- */
            if (is_reading_active) {
                char *uart_buf = malloc(strlen(current_text) + 10);
                if (uart_buf) {
                    sprintf(uart_buf, "NOV:%s", current_text);
                    my_uart_send(uart_buf);
                    free(uart_buf);
                }
            }

            /* ---- TTS 合成并播放 ---- */
            ESP_LOGI(TAG, "▶️ 开始合成并播放: %s", current_text);

            // 开始中文文本合成
            if (esp_tts_parse_chinese(tts_handle, current_text)) {
                int len[1] = {0};
                do {
                    // 检查打断标志 (用户可能在此期间退出了)
                    if (force_stop_tts) break;

                    // 获取一帧 PCM 音频数据
                    short *pcm = esp_tts_stream_play(tts_handle, len, global_tts_speed);
                    if (pcm != NULL && len[0] > 0) {
                        // 通过扬声器播放 (len[0]*2 = 字节数，16-bit 采样)
                        playSpeaker((uint8_t *)pcm, len[0] * 2);
                    }
                    vTaskDelay(pdMS_TO_TICKS(2));  // 短暂让出 CPU
                } while (len[0] > 0);  // len[0]==0 表示合成完成

                // 重置 TTS 流状态
                esp_tts_stream_reset(tts_handle);
            }

            // 释放扬声器互斥锁
            xSemaphoreGive(speaker_mutex);

            // 释放文本内存 (由 tts_speak 中 strdup 分配)
            free(current_text);
            current_text = NULL;

            /* ---- 小说模式：请求下一页 ---- */
            // 如果在读小说模式下，当前句子播放完毕且队列为空，
            // 立即发信号给 novel_read_task 请求读取下一段
            if (is_reading_active && !force_stop_tts && uxQueueMessagesWaiting(tts_queue) == 0) {
                extern SemaphoreHandle_t next_page_sem;
                if (next_page_sem != NULL) {
                    xSemaphoreGive(next_page_sem);  // 释放信号量，唤醒小说读取任务
                }
            }
        }
    }
}

/* =====================================================================
 * TTS 引擎初始化
 * =====================================================================
 * @brief 初始化 ESP-TTS 引擎，加载语音模型，启动合成任务
 *
 * 初始化步骤：
 *   1. 创建文本消息队列 (容量 10 个句子)
 *   2. 从 SD 卡加载语音模型到 PSRAM
 *   3. 初始化 TTS 语音集 (使用"小乐"中文语音)
 *   4. 创建 TTS 引擎句柄
 *   5. 启动 tts_main_task 任务 (Core 0, 32KB 栈)
 */
void init_tts_engine() {
    /* ---- 步骤1: 创建消息队列 ---- */
    tts_queue = xQueueCreate(10, sizeof(char *));  // 最多缓存 10 个句子

    /* ---- 步骤2: 从 SD 卡加载语音模型 ---- */
    if (load_tts_model_from_sd(MOUNT_POINT "/esp_tts_voice_data_xiaole.dat") != ESP_OK) {
        ESP_LOGE(TAG, "❌ TTS 模型加载失败，TTS 功能不可用");
        return;
    }

    /* ---- 步骤3: 初始化语音集 ---- */
    // esp_tts_voice_xiaole 是编译时内置的中文语音定义
    // model_data_in_psram 是从 SD 卡加载的语音数据
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, (void *)model_data_in_psram);

    /* ---- 步骤4: 创建 TTS 引擎句柄 ---- */
    tts_handle = esp_tts_create(voice);

    if (tts_handle == NULL) {
        ESP_LOGE(TAG, "❌ TTS 句柄创建失败！");
    } else {
        ESP_LOGI(TAG, "🚀 TTS 系统初始化成功");

        /* ---- 步骤5: 启动合成任务 ---- */
        // 栈大小 32KB：TTS 合成需要大量栈空间，之前 8KB 会导致栈溢出
        // 绑定到 Core 0：与音频 Hub (Core 1) 分离
        xTaskCreatePinnedToCore(tts_main_task, "tts_task", 32768, NULL, 5, NULL, 0);
    }
}

/* =====================================================================
 * TTS 文本输入接口 (带自动切句)
 * =====================================================================
 * @brief 将文本送入 TTS 队列，自动按标点切句防止过载
 *
 * 切句规则：
 *   - 最大句子长度: 90 字节
 *   - 优先在中/英文标点处切句
 *   - 如果没有标点但超过 90 字节，强制切句
 *   - 每个句子通过 strdup 复制后放入队列
 *
 * @param text 要合成的 UTF-8 中文文本
 */
void tts_speak(const char *text) {
    if (text == NULL || tts_queue == NULL) return;

    const int MAX_CHUNK_BYTES = 90;  // 每个句子的最大字节数
    char chunk_buf[MAX_CHUNK_BYTES + 4];  // 临时缓冲区 (+4 保险)
    int chunk_len = 0;  // 当前句子已累积的字节数

    const char *src = text;  // 源文本指针

    while (*src) {
        /* ---- 解析当前字符的 UTF-8 字节长度 ---- */
        int char_bytes = 1;
        unsigned char c = (unsigned char)*src;
        if (c < 0x80) char_bytes = 1;        // ASCII (0xxxxxxx)
        else if (c < 0xE0) char_bytes = 2;   // 2字节 (110xxxxx)
        else if (c < 0xF0) char_bytes = 3;   // 3字节 (1110xxxx) - 大部分中文
        else char_bytes = 4;                  // 4字节 (11110xxx)

        /* ---- 检查是否是标点符号 ---- */
        int is_punc = 0;
        int zh_punc_len = 0;

        if (char_bytes == 1 && (ispunct(c) || iscntrl(c) || isspace(c))) {
            is_punc = 1;  // 英文标点/控制字符/空格
        } else if (char_bytes >= 3) {
            zh_punc_len = skip_zh_punctuation(src);
            if (zh_punc_len > 0) is_punc = 1;  // 中文标点
        }

        if (is_punc) {
            /* ---- 遇到标点：切句 ---- */
            if (chunk_len > 0) {
                chunk_buf[chunk_len] = '\0';
                // 复制句子并放入队列
                char *text_copy = strdup(chunk_buf);
                if (text_copy) {
                    if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) {
                        free(text_copy);  // 队列满时释放
                    }
                }
                chunk_len = 0;  // 重置
            }
            // 跳过标点符号本身 (中文标点 3 字节，英文标点 1 字节)
            src += (zh_punc_len > 0) ? zh_punc_len : 1;
        } else {
            /* ---- 普通字符：追加到当前句子 ---- */
            // 如果追加后会超过最大长度，先切句
            if (chunk_len + char_bytes > MAX_CHUNK_BYTES) {
                chunk_buf[chunk_len] = '\0';
                char *text_copy = strdup(chunk_buf);
                if (text_copy) {
                    if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) {
                        free(text_copy);
                    }
                }
                chunk_len = 0;
            }

            // 将当前字符追加到缓冲区
            for (int i = 0; i < char_bytes; i++) {
                chunk_buf[chunk_len++] = src[i];
            }
            src += char_bytes;
        }
    }

    /* ---- 处理最后一个不以标点结尾的句子 ---- */
    if (chunk_len > 0) {
        chunk_buf[chunk_len] = '\0';
        char *text_copy = strdup(chunk_buf);
        if (text_copy) {
            if (xQueueSend(tts_queue, &text_copy, portMAX_DELAY) != pdTRUE) {
                free(text_copy);
            }
        }
    }
}

/* =====================================================================
 * 兼容旧接口名
 * =====================================================================
 * @brief jarvis_add_text 是旧版本的接口名，现在内部调用 tts_speak
 * @param new_text 要合成的文本
 */
void jarvis_add_text(const char *new_text) {
    tts_speak(new_text);
}
