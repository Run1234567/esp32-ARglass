/**
 * @file voice_app.c
 * @brief "Jarvis" 唤醒词检测模块 (ESP-SR WakeNet9 + AFE)
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 使用乐鑫 ESP-SR (Speech Recognition) 库实现语音唤醒词检测。
 * 系统持续监听麦克风音频，当检测到 "Jarvis" (贾维斯) 唤醒词时触发响应。
 *
 * 技术架构：
 *   PDM麦克风 -> audio_hub_task -> sr_ringbuf -> feed_audio_task -> AFE -> detect_task
 *
 * 核心组件：
 *   1. AFE (Audio Front End) - 音频前端
 *      - 降噪处理
 *      - 回声消除 (AEC)
 *      - 音频帧分割
 *
 *   2. WakeNet9 - 唤醒词神经网络
 *      - 专门训练的 "Jarvis" 唤醒词模型
 *      - 模型文件存储在 SPIFFS 分区 ("model" 分区, 4MB)
 *      - 低阈值 (0.1) 设置，对口音友好
 *
 * 两个 FreeRTOS 任务 (都运行在 Core 0):
 *   - feed_audio_task: 从 sr_ringbuf 读取音频，x4 软件放大后喂给 AFE
 *   - detect_task:     从 AFE 获取处理结果，检测唤醒词事件
 *
 * 依赖组件：
 *   - esp-sr:     ESP 语音识别库 (WakeNet9 + AFE)
 *   - freertos:   任务/信号量
 *   - esp_ringbuf: 环形缓冲区
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"

/* ==================== ESP-SR 核心头文件 ==================== */
#include "esp_wn_iface.h"       // WakeNet 接口定义
#include "esp_wn_models.h"      // WakeNet 模型加载
#include "esp_afe_sr_iface.h"   // AFE (Audio Front End) 接口
#include "esp_afe_sr_models.h"  // AFE 模型配置

// 注意：已彻底删除所有 esp_mn_xxx (MultiNet) 的头文件
// MultiNet 用于语音命令识别，当前版本仅使用唤醒词检测

static const char *TAG = "VOICE_APP";  // 日志标签

/* =====================================================================
 * 外部引用
 * ===================================================================== */
extern RingbufHandle_t sr_ringbuf;  // 语音识别专属环形缓冲区 (在 main.c 中创建)

/**
 * @brief ESP-SR 模型列表初始化函数 (外部声明)
 * @param partition_label SPIFFS 分区标签 ("model")
 * @return 模型列表指针
 */
extern srmodel_list_t *esp_srmodel_init(const char *partition_label);

/* =====================================================================
 * 全局句柄
 * ===================================================================== */
static esp_afe_sr_iface_t *afe_handle = NULL;  // AFE 接口句柄
static esp_afe_sr_data_t *afe_data = NULL;     // AFE 运行时数据

/* =====================================================================
 * 音频喂入任务 (Feed Task)
 * =====================================================================
 * @brief 从 sr_ringbuf 读取音频数据，放大后喂给 AFE 处理
 *
 * 工作流程：
 *   1. 从 sr_ringbuf 读取音频片段
 *   2. 累积到 AFE 需要的帧大小 (chunk_size 个采样点)
 *   3. 对每个采样点进行 x4 软件放大
 *      (PDM 麦克风灵敏度较低，需要软件增益补偿)
 *   4. 将放大后的音频帧喂给 AFE
 *
 * 为什么需要 x4 放大？
 *   PDM 麦克风的输出电平通常较低，直接输入 AFE 会导致
 *   唤醒率下降。x4 放大 (+12dB) 可以显著提高检测灵敏度。
 *   同时做了防溢出裁剪 (Clipping)。
 *
 * 优先级: 5
 * 栈大小: 8192 字节
 * 核心绑定: Core 0
 */
static void feed_audio_task(void *arg) {
    // 获取 AFE 每帧需要的采样点数
    int chunk_size = afe_handle->get_feed_chunksize(afe_data);
    size_t chunk_bytes = chunk_size * sizeof(int16_t);  // 每帧字节数

    // 分配帧缓冲区 (堆上分配，防止栈溢出)
    int16_t *feed_buffer = malloc(chunk_bytes);
    size_t current_bytes_in_buffer = 0;  // 当前缓冲区已累积的字节数
    size_t item_size = 0;

    ESP_LOGI(TAG, "🎙️ 纯净版唤醒搬运工已就绪...");

    while (1) {
        // 从环形缓冲区读取音频数据 (无限等待)
        void *audio_data = xRingbufferReceive(sr_ringbuf, &item_size, portMAX_DELAY);
        if (audio_data != NULL) {
            uint8_t *src_ptr = (uint8_t *)audio_data;
            size_t bytes_left_to_process = item_size;

            // 将接收到的数据逐块拷贝到帧缓冲区
            while (bytes_left_to_process > 0) {
                size_t space_left = chunk_bytes - current_bytes_in_buffer;
                size_t copy_size = (bytes_left_to_process < space_left) ?
                                   bytes_left_to_process : space_left;

                memcpy((uint8_t *)feed_buffer + current_bytes_in_buffer, src_ptr, copy_size);

                current_bytes_in_buffer += copy_size;
                src_ptr += copy_size;
                bytes_left_to_process -= copy_size;

                // 帧缓冲区满，开始处理
                if (current_bytes_in_buffer >= chunk_bytes) {
                    /* ---- 音频 x4 软件放大器 ---- */
                    // 将每个采样点放大 4 倍 (+12dB)
                    for (int i = 0; i < chunk_size; i++) {
                        int32_t sample = feed_buffer[i] * 4;
                        // 防溢出裁剪 (Clipping)
                        if (sample > 32767) sample = 32767;
                        if (sample < -32768) sample = -32768;
                        feed_buffer[i] = (int16_t)sample;
                    }

                    // 将放大后的音频帧喂给 AFE (降噪 + 唤醒检测)
                    afe_handle->feed(afe_data, feed_buffer);
                    current_bytes_in_buffer = 0;  // 重置缓冲区
                }
            }
            // 归还环形缓冲区内存
            vRingbufferReturnItem(sr_ringbuf, audio_data);
        }
    }
}

/* =====================================================================
 * 唤醒词检测任务 (Detect Task)
 * =====================================================================
 * @brief 极简的 WakeNet 监听循环，检测 "Jarvis" 唤醒词
 *
 * 工作流程：
 *   1. 调用 afe_handle->fetch() 获取 AFE 处理结果
 *   2. 检查 res->wakeup_state 是否为 WAKENET_DETECTED
 *   3. 如果检测到唤醒词，打印成功日志
 *
 * TODO (未来扩展):
 *   - 播放提示音 (告诉用户"我在听")
 *   - AR 屏幕闪烁反馈
 *   - 开启 WebSocket 流式传输接下来的语音到服务器
 *   - 接收服务器返回的语音识别结果并执行命令
 *
 * 优先级: 5
 * 栈大小: 8192 字节
 * 核心绑定: Core 0
 */
static void detect_task(void *arg) {
    ESP_LOGI(TAG, "🧠 AI 唤醒引擎正在监听 (内存自由模式)...");

    while (1) {
        // 从 AFE 获取处理结果 (阻塞式)
        afe_fetch_result_t* res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;

        // 检查是否检测到唤醒词
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "=================================");
            ESP_LOGI(TAG, "🚀 [成功] 识别到唤醒词：贾维斯！");
            ESP_LOGI(TAG, "=================================");

            // 💡 这里是未来扩展点：
            // TODO 1: 播放一个提示音
            // TODO 2: AR 屏幕显示"聆听中"动画
            // TODO 3: 开始将后续语音通过 WebSocket 发送给服务器
            // TODO 4: 接收并执行服务器返回的命令
        }
    }
}

/* =====================================================================
 * 启动唤醒引擎
 * =====================================================================
 * @brief 初始化 ESP-SR 模型、AFE 和 WakeNet，启动检测任务
 *
 * 初始化步骤：
 *   1. 从 "model" SPIFFS 分区加载模型列表
 *   2. 筛选 WakeNet 唤醒词模型
 *   3. 初始化 AFE (音频前端)
 *      - 模式: "M" (麦克风模式)
 *      - 类型: AFE_TYPE_SR (语音识别)
 *      - 性能: AFE_MODE_HIGH_PERF (高性能模式)
 *   4. 设置低唤醒阈值 (0.1)，对各种口音友好
 *   5. 启动 feed_audio_task 和 detect_task
 */
void start_jarvis_brain(void) {
    ESP_LOGI(TAG, "正在启动 AI 唤醒引擎 (仅 WakeNet)...");

    /* ---- 步骤1: 初始化模型列表 ---- */
    // 从 SPIFFS 分区 "model" 加载所有可用的 AI 模型
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "❌ 模型初始化失败！");
        return;
    }

    /* ---- 步骤2: 筛选 WakeNet 唤醒词模型 ---- */
    // ESP_WN_PREFIX 是 WakeNet 模型的前缀标识
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (wn_name == NULL) {
        ESP_LOGE(TAG, "❌ 分区内未找到 WakeNet 唤醒模型！");
        return;
    }
    ESP_LOGI(TAG, "🔍 成功加载唤醒模型: %s", wn_name);

    /* ---- 步骤3: 初始化 AFE (音频前端) ---- */
    // 参数: "M"=麦克风模式, models=模型列表, AFE_TYPE_SR=语音识别, HIGH_PERF=高性能
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->wakenet_model_name = wn_name;  // 指定唤醒词模型

    // 去掉 MultiNet 后 PSRAM 极度宽裕，使用默认内存分配
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    // 创建 AFE 句柄和运行时数据
    afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);

    /* ---- 步骤4: 设置唤醒阈值 ---- */
    // 阈值 0.1 (默认约 0.5)：极低的阈值对各种口音极其友好
    // 代价是可能有少量误唤醒，但在 AR 眼镜场景下可以接受
    afe_handle->set_wakenet_threshold(afe_data, 1, 0.1);

    /* ---- 步骤5: 启动 FreeRTOS 任务 ---- */
    // feed_audio_task: 从环形缓冲区读取音频，放大后喂给 AFE
    xTaskCreatePinnedToCore(feed_audio_task, "voice_feed", 8192, NULL, 5, NULL, 0);
    // detect_task: 从 AFE 获取结果，检测唤醒词
    xTaskCreatePinnedToCore(detect_task,   "voice_detect", 8192, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "✅ 唤醒引擎启动完毕！系统内存现在极度安全。");
}
