/**
 * @file camera_app.c
 * @brief 摄像头初始化、拍照与智能视频推流模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 管理 OV2640 并行接口摄像头，提供初始化、拍照、保存到 SD 卡等功能。
 * 摄像头工作在 JPEG/UXGA (1600x1200) 模式，图像直接由硬件编码为 JPEG，
 * 帧缓冲存储在 PSRAM 中 (2 个缓冲区，双缓冲)。
 *
 *   1. 拍照功能
 *      - UXGA 高分辨率拍照 (1600x1200)
 *      - 自动保存到 SD 卡
 *      - 可选上传到服务器
 *
 *   2. 视频推流 (智能弱网优化)
 *      - 自适应分辨率 (VGA/SVGA/XGA)
 *      - 自适应帧率 (10/15/20/25 FPS)
 *      - 网络状态感知 (自动暂停/恢复)
 *      - 本地缓存 (网络差时保存到SD卡)
 *      - 离线模式 (完全断网时本地预览)
 *
 * GPIO 引脚映射 (16-bit 并行数据总线 + 控制信号):
 *   数据总线 D0-D7:  GPIO 15, 17, 18, 16, 14, 12, 11, 48
 *   像素时钟 PCLK:   GPIO 13
 *   水平同步 HREF:   GPIO 47
 *   垂直同步 VSYNC:  GPIO 38
 *   主时钟 XCLK:     GPIO 10
 *   SCCB 数据 SIOD:  GPIO 40
 *   SCCB 时钟 SIOC:  GPIO 39
 *   电源控制 PWDN:   -1 (未使用)
 *   复位 RESET:      -1 (未使用)
 *
 * 依赖组件：
 *   - esp32-camera: 乐鑫摄像头驱动库
 *   - sd_card_app:  SD 卡挂载点定义 (MOUNT_POINT)
 *   - my_uart:      串口通信 (通知 UI 拍照完成)
 *   - log/driver:   日志和底层驱动
 *   - esp_http_client: HTTP客户端 (上传照片)
 *   - lwip/sockets: TCP Socket (视频推流)
 */

#include "camera_app.h"
#include "esp_camera.h"       // ESP32 摄像头驱动核心 API
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "my_uart.h"          // 串口通信模块
#include "driver/uart.h"
#include <stdio.h>
#include <sys/stat.h>         // mkdir 等文件系统操作
#include "sd_card_app.h"      // MOUNT_POINT 定义
#include "esp_http_client.h"  // HTTP 客户端 (用于上传照片)
#include "lwip/sockets.h"    // TCP Socket (用于视频推流)
#include "esp_timer.h"       // 高精度时间戳 (用于视频帧时间标记)

static const char *TAG = "CAMERA_APP";  // 日志标签

/**
 * @brief 拍照状态标志
 * true = 正在拍照中 (其他模块可以据此避免冲突)
 * 由 execute_high_res_capture() 设置/清除
 */
volatile bool is_capturing = false;

/**
 * @brief 视频推流状态标志
 * 由 UI MCU 通过 UART 命令控制:
 *   CMD:VIDEO_START -> is_video_recording = true
 *   CMD:VIDEO_STOP  -> is_video_recording = false
 */
volatile bool is_video_recording = false;

/* =====================================================================
 * 弱网视频推流配置
 * ===================================================================== */
// 视频服务器配置
#define VIDEO_SERVER_IP   "124.220.224.189"
#define VIDEO_SERVER_PORT 8890

// 自适应分辨率配置
#define VIDEO_QUALITY_LOW     28    // 低画质 (弱网)
#define VIDEO_QUALITY_MEDIUM  22    // 中等画质 (中等网络)
#define VIDEO_QUALITY_HIGH    18    // 高画质 (良好网络)

// 自适应帧率配置 (毫秒/帧)
#define VIDEO_FPS_LOW     100    // 10 FPS (弱网)
#define VIDEO_FPS_MEDIUM  66     // 15 FPS (中等网络)
#define VIDEO_FPS_HIGH    40     // 25 FPS (良好网络)

// 网络检测配置
#define NETWORK_CHECK_INTERVAL_MS    5000   // 网络检测间隔 (5秒)
#define NETWORK_STABLE_THRESHOLD     3      // 网络稳定阈值 (连续3次成功)

// 本地缓存配置
#define VIDEO_CACHE_DIR              "/sdcard/video_cache"
#define VIDEO_MAX_CACHE_FILES        100    // 最大缓存文件数

/* =====================================================================
 * 高分辨率拍照执行函数
 * =====================================================================
 * @brief 完整的拍照工作流：切换到高分辨率 -> 预热 -> 拍照 -> 恢复
 *
 * 流程：
 *   1. 设置 is_capturing 标志，防止其他模块干扰
 *   2. 切换到 JPEG/UXGA (1600x1200) 高分辨率模式
 *   3. 等待 500ms 让传感器稳定
 *   4. 丢弃前 2 帧 (预热帧，可能曝光不准)
 *   5. 执行实际拍照并保存到 SD 卡
 *   6. 恢复 UXGA/JPEG 模式
 *   7. 通过 UART 通知 UI 拍照完成
 *
 * 注意：此函数耗时约 1 秒，不应在实时性要求高的任务中调用
 */
void execute_high_res_capture(void) {
    is_capturing = true;
    vTaskDelay(pdMS_TO_TICKS(50));  // 短暂延时，让其他任务感知到状态变化

    /* ---- 步骤1: 切换到高分辨率模式 ---- */
    sensor_t *s = esp_camera_sensor_get();           // 获取传感器控制句柄
    s->set_pixformat(s, PIXFORMAT_JPEG);             // JPEG 压缩格式
    s->set_framesize(s, FRAMESIZE_UXGA);             // UXGA: 1600x1200
    s->set_quality(s, 12);                           // JPEG 质量 (0-63, 越低越好)
    vTaskDelay(pdMS_TO_TICKS(500));                  // 等待传感器稳定

    /* ---- 步骤2: 预热 - 丢弃前 2 帧 ---- */
    // 前几帧可能曝光不准确或有伪影，需要丢弃
    for (int i = 0; i < 2; i++) {
        camera_fb_t *dummy_fb = esp_camera_fb_get();
        if (dummy_fb) esp_camera_fb_return(dummy_fb);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ---- 步骤3: 实际拍照并保存 ---- */
    extern void take_photo_to_PZ_folder(void);
    take_photo_to_PZ_folder();

    /* ---- 步骤4: 恢复模式设置 ---- */
    s->set_framesize(s, FRAMESIZE_UXGA);
    s->set_pixformat(s, PIXFORMAT_JPEG);

    is_capturing = false;

    /* ---- 步骤5: 通知 UI 拍照完成 ---- */
    my_uart_send("CMD:PHOTO_DONE\r\n");
}

/* =====================================================================
 * 摄像头 GPIO 引脚定义
 * =====================================================================
 * OV2640 并行接口需要 8 根数据线 + 5 根控制线 + 2 根 SCCB 线
 * 这些引脚由 PCB 布局决定，不可随意更改
 */
#define PWDN_GPIO_NUM     -1    // 电源控制 (未使用)
#define RESET_GPIO_NUM    -1    // 复位 (未使用)
#define XCLK_GPIO_NUM     10    // 主时钟输出 (ESP32 提供给摄像头)
#define SIOD_GPIO_NUM     40    // SCCB 数据线 (I2C 兼容，用于配置寄存器)
#define SIOC_GPIO_NUM     39    // SCCB 时钟线
#define Y9_GPIO_NUM       48    // 数据总线 D7
#define Y8_GPIO_NUM       11    // 数据总线 D6
#define Y7_GPIO_NUM       12    // 数据总线 D5
#define Y6_GPIO_NUM       14    // 数据总线 D4
#define Y5_GPIO_NUM       16    // 数据总线 D3
#define Y4_GPIO_NUM       18    // 数据总线 D2
#define Y3_GPIO_NUM       17    // 数据总线 D1
#define Y2_GPIO_NUM       15    // 数据总线 D0
#define VSYNC_GPIO_NUM    38    // 垂直同步信号
#define HREF_GPIO_NUM     47    // 水平参考信号
#define PCLK_GPIO_NUM     13    // 像素时钟

/* =====================================================================
 * 摄像头初始化函数
 * =====================================================================
 * @brief 初始化 OV2640 并行摄像头，配置为 JPEG/UXGA 模式
 *
 * 配置要点：
 *   - 主时钟: 20MHz (XCLK)
 *   - 像素格式: JPEG (硬件压缩)
 *   - 分辨率: UXGA (1600x1200)
 *   - JPEG 质量: 12 (中等偏高)
 *   - 帧缓冲位置: PSRAM (大容量，适合高分辨率图像)
 *   - 帧缓冲数量: 2 (双缓冲，提高采集效率)
 *   - 采集模式: CAMERA_GRAB_WHEN_EMPTY (缓存空时采集)
 *
 * 初始化后会预热 3 帧，确保摄像头工作正常
 */
void initCamera(void) {
    camera_config_t config;

    /* ---- LEDC 配置 (用于生成 XCLK 主时钟) ---- */
    config.ledc_channel = LEDC_CHANNEL_0;  // 使用 LEDC 通道 0
    config.ledc_timer = LEDC_TIMER_0;      // 使用 LEDC 定时器 0

    /* ---- 数据总线引脚 (D0-D7) ---- */
    config.pin_d0 = Y2_GPIO_NUM;   // GPIO 15
    config.pin_d1 = Y3_GPIO_NUM;   // GPIO 17
    config.pin_d2 = Y4_GPIO_NUM;   // GPIO 18
    config.pin_d3 = Y5_GPIO_NUM;   // GPIO 16
    config.pin_d4 = Y6_GPIO_NUM;   // GPIO 14
    config.pin_d5 = Y7_GPIO_NUM;   // GPIO 12
    config.pin_d6 = Y8_GPIO_NUM;   // GPIO 11
    config.pin_d7 = Y9_GPIO_NUM;   // GPIO 48

    /* ---- 控制信号引脚 ---- */
    config.pin_xclk = XCLK_GPIO_NUM;     // 主时钟: GPIO 10
    config.pin_pclk = PCLK_GPIO_NUM;     // 像素时钟: GPIO 13
    config.pin_vsync = VSYNC_GPIO_NUM;   // 垂直同步: GPIO 38
    config.pin_href = HREF_GPIO_NUM;     // 水平参考: GPIO 47

    /* ---- SCCB (I2C) 引脚 ---- */
    config.pin_sccb_sda = SIOD_GPIO_NUM;  // 数据线: GPIO 40
    config.pin_sccb_scl = SIOC_GPIO_NUM;  // 时钟线: GPIO 39

    /* ---- 电源和复位 ---- */
    config.pin_pwdn = PWDN_GPIO_NUM;    // -1 (未使用)
    config.pin_reset = RESET_GPIO_NUM;  // -1 (未使用)

    /* ---- 时钟频率 ---- */
    config.xclk_freq_hz = 20000000;  // 20MHz 主时钟

    /* ---- 图像参数 ---- */
    config.frame_size = FRAMESIZE_UXGA;        // 1600x1200 分辨率
    config.pixel_format = PIXFORMAT_JPEG;      // JPEG 硬件压缩格式
    config.grab_mode = CAMERA_GRAB_LATEST;     // 疯狂连拍模式，始终采集最新画面
    config.fb_location = CAMERA_FB_IN_PSRAM;   // 帧缓冲存放在 PSRAM
    config.fb_count = 2;                       // 双缓冲
    config.jpeg_quality = 12;                  // JPEG 质量 (0-63)

    /* ---- 初始化摄像头驱动 ---- */
    ESP_ERROR_CHECK(esp_camera_init(&config));

    ESP_LOGI(TAG, "📷 摄像头初始化完成 (JPEG 模式，仅拍照功能)");

    /* ---- 预热 3 帧 ---- */
    // 丢弃前几帧，让自动曝光/白平衡稳定
    for (int i = 0; i < 3; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "📷 摄像头预热完成");
}

/* =====================================================================
 * 照片上传到云端服务器
 * =====================================================================
 * @brief 将摄像头拍摄的 JPEG 图片通过 HTTP POST 上传到服务器
 *
 * 使用 multipart/form-data 格式，直接从 PSRAM 帧缓冲读取数据，
 * 不需要先保存到 SD 卡再读取。
 *
 * @param pic 摄像头帧缓冲指针
 */
#define PHOTO_UPLOAD_URL "http://124.220.224.189:5000/upload_image"

static void upload_photo_to_server(camera_fb_t *pic) {
    if (!pic) return;

    ESP_LOGI(TAG, "🚀 开始上传照片到服务器... 大小: %d 字节", pic->len);

    esp_http_client_config_t config = {
        .url = PHOTO_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,  // UXGA 图片较大，15 秒超时
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        return;
    }

    // 构建 multipart/form-data 报文
    const char *boundary = "----Esp32CameraBoundary";

    char header[256];
    snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"esp32_photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n", boundary);

    char footer[64];
    snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    int total_len = strlen(header) + pic->len + strlen(footer);

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ HTTP 连接失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // 分段写入：表头 → 图片二进制 → 表尾
    esp_http_client_write(client, header, strlen(header));
    esp_http_client_write(client, (const char *)pic->buf, pic->len);
    esp_http_client_write(client, footer, strlen(footer));

    // 读取服务器响应
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code == 200) {
        ESP_LOGI(TAG, "✅ 照片上传成功！状态码: %d", status_code);
    } else {
        ESP_LOGE(TAG, "⚠️ 上传异常，状态码: %d", status_code);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* =====================================================================
 * 弱网环境辅助函数
 * =====================================================================
 */

/**
 * @brief 检查网络是否可用 (使用HTTP测试)
 * @return true 网络可用, false 网络不可用
 *
 * 使用轻量级HTTP HEAD请求测试网络连接性
 * 不依赖esp_netif.h，兼容所有ESP-IDF版本
 */
static bool is_network_available(void) {
    // 使用上传服务器作为测试目标
    esp_http_client_config_t config = {0};
    config.url = "http://124.220.224.189:5000/";
    config.method = HTTP_METHOD_HEAD;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        return (status_code > 0);
    }

    esp_http_client_cleanup(client);
    return false;
}

/**
 * @brief 保存视频帧到SD卡缓存
 * @param fb 帧缓冲指针
 * @param timestamp 时间戳
 * @return ESP_OK 成功, ESP_FAIL 失败
 *
 * 网络不可用时，将视频帧保存到SD卡
 * 等待网络恢复后可以手动上传
 */
static esp_err_t save_frame_to_cache(camera_fb_t *fb, uint32_t timestamp) {
    if (!fb || !fb->buf) {
        return ESP_FAIL;
    }

    // 创建缓存目录
    mkdir(VIDEO_CACHE_DIR, 0777);

    // 生成文件名 (使用时间戳避免冲突)
    char file_path[128];
    snprintf(file_path, sizeof(file_path), "%s/VID_%lu.jpg", VIDEO_CACHE_DIR, timestamp);

    FILE *file = fopen(file_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "❌ 无法创建缓存文件: %s", file_path);
        return ESP_FAIL;
    }

    // 写入JPEG数据
    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fclose(file);

    if (written == fb->len) {
        ESP_LOGD(TAG, "💾 视频帧已缓存: %s (%d bytes)", file_path, fb->len);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "❌ 缓存文件写入失败: %s", file_path);
        remove(file_path);
        return ESP_FAIL;
    }
}

/**
 * @brief 自适应调整视频质量
 * @param network_stable 网络是否稳定
 * @param current_quality 当前质量设置
 * @return 新的质量设置
 *
 * 根据网络状况动态调整视频质量:
 * - 网络稳定: 提高画质
 * - 网络不稳定: 降低画质
 */
static int adaptive_quality_adjustment(bool network_stable, int current_quality) {
    if (network_stable) {
        // 网络稳定，尝试提高画质 (降低quality值)
        if (current_quality > VIDEO_QUALITY_HIGH) {
            return current_quality - 2;  // 逐步提高
        }
        return VIDEO_QUALITY_HIGH;
    } else {
        // 网络不稳定，降低画质 (增加quality值)
        if (current_quality < VIDEO_QUALITY_LOW) {
            return current_quality + 2;  // 逐步降低
        }
        return VIDEO_QUALITY_LOW;
    }
}

/* =====================================================================
 * 视频推流任务 (TCP 端口 8890) - 弱网优化版
 * =====================================================================
 * @brief 将摄像头 JPEG 帧通过 TCP 实时推送到云服务器
 *
 * 协议: [4字节长度 (小端序)] + [4字节时间戳] + [JPEG 数据]
 * 分辨率: VGA (640x480) 以提高帧率
 *
 * 弱网优化:
 *   - 自适应画质 (根据网络状况动态调整)
 *   - 自适应帧率 (网络差时降低帧率)
 *   - 本地缓存 (网络断开时保存到SD卡)
 *   - 网络检测 (定期检查网络状态)
 *
 * 工作模式:
 *   - is_video_recording = false: 休眠待机
 *   - is_video_recording = true:  连接服务器，持续推流
 *   - 网络断开: 自动保存到本地缓存
 *   - 推流结束: 恢复 UXGA 高分辨率拍照模式
 *
 * 优先级: 4
 * 栈大小: 8192 字节
 * 核心绑定: Core 1
 */
#define VIDEO_SERVER_IP   "124.220.224.189"
#define VIDEO_SERVER_PORT 8890

void video_stream_task(void *pvParameters) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(VIDEO_SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(VIDEO_SERVER_PORT);

    // 弱网优化变量
    int current_quality = VIDEO_QUALITY_MEDIUM;  // 当前画质
    int current_fps_delay = VIDEO_FPS_MEDIUM;    // 当前帧间隔
    bool network_stable = true;                  // 网络是否稳定
    int network_check_counter = 0;               // 网络检测计数器
    int send_fail_count = 0;                     // 发送失败计数

    while (1) {
        // 休眠等待 UI 触发
        while (!is_video_recording) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "🎥 开始视频推流 (弱网优化模式)...");

        // 切换到 VGA 分辨率 + 中等画质
        sensor_t *s = esp_camera_sensor_get();
        s->set_framesize(s, FRAMESIZE_VGA);
        s->set_quality(s, current_quality);

        // 丢弃前 5 帧，确保传感器输出已经是 VGA 数据
        for (int i = 0; i < 5; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        ESP_LOGI(TAG, "VGA 切换完成，开始推流...");

        // 建立 TCP 连接 (带重试)
        int sock = -1;
        int connect_retry = 0;
        bool connected = false;

        while (!connected && connect_retry < 3) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                ESP_LOGE(TAG, "❌ Socket 创建失败，重试 %d/3", connect_retry + 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                connect_retry++;
                continue;
            }

            if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
                connected = true;
                ESP_LOGI(TAG, "✅ 成功连接到视频服务器 %s:%d", VIDEO_SERVER_IP, VIDEO_SERVER_PORT);
            } else {
                ESP_LOGW(TAG, "⚠️ 连接视频服务器失败，重试 %d/3", connect_retry + 1);
                close(sock);
                sock = -1;
                connect_retry++;
                vTaskDelay(pdMS_TO_TICKS(2000));  // 等待2秒后重试
            }
        }

        if (!connected) {
            ESP_LOGE(TAG, "❌ 无法连接视频服务器，进入离线模式");

            // 离线模式：持续保存到本地缓存
            while (is_video_recording) {
                camera_fb_t *pic = esp_camera_fb_get();
                if (pic) {
                    uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000);
                    save_frame_to_cache(pic, timestamp);
                    esp_camera_fb_return(pic);
                }
                vTaskDelay(pdMS_TO_TICKS(VIDEO_FPS_LOW));  // 低帧率保存
            }

            // 离线模式结束，恢复拍照模式
            s->set_framesize(s, FRAMESIZE_UXGA);
            s->set_quality(s, 12);
            ESP_LOGI(TAG, "⏹️ 离线录制结束");
            continue;  // 继续等待下次触发
        }

        // 成功连接，开始推流
        send_fail_count = 0;  // 重置失败计数

        while (is_video_recording) {
            // 定期检查网络状态
            if (++network_check_counter >= (NETWORK_CHECK_INTERVAL_MS / current_fps_delay)) {
                network_check_counter = 0;
                bool new_network_status = is_network_available();

                if (new_network_status != network_stable) {
                    network_stable = new_network_status;
                    current_quality = adaptive_quality_adjustment(network_stable, current_quality);
                    s->set_quality(s, current_quality);

                    if (network_stable) {
                        ESP_LOGI(TAG, "📶 网络恢复，提高画质: quality=%d", current_quality);
                        current_fps_delay = VIDEO_FPS_HIGH;
                    } else {
                        ESP_LOGW(TAG, "📶 网络不稳定，降低画质: quality=%d", current_quality);
                        current_fps_delay = VIDEO_FPS_LOW;
                    }
                }
            }

            // 抓取一帧 JPEG
            camera_fb_t *pic = esp_camera_fb_get();
            if (!pic) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // 按协议发送: [4字节长度] + [4字节时间戳] + [JPEG数据]
            uint32_t len = pic->len;
            uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000);  // 微秒转毫秒

            int send_res = send(sock, &len, 4, 0);          // 1. 发送 4 字节长度
            if (send_res >= 0) {
                send_res = send(sock, &timestamp, 4, 0);     // 2. 发送 4 字节时间戳
            }
            if (send_res >= 0) {
                send_res = send(sock, pic->buf, pic->len, 0); // 3. 发送 JPEG 数据
            }

            if (send_res < 0) {
                send_fail_count++;
                ESP_LOGW(TAG, "⚠️ 发送失败 (%d/5)", send_fail_count);

                // 连续失败5次，认为网络断开
                if (send_fail_count >= 5) {
                    ESP_LOGE(TAG, "❌ 网络断开，切换到本地缓存模式");
                    network_stable = false;
                    break;
                }
            } else {
                send_fail_count = 0;  // 成功发送，重置计数
            }

            // 保存帧到本地缓存 (网络不稳定时)
            if (!network_stable) {
                save_frame_to_cache(pic, timestamp);
            }

            esp_camera_fb_return(pic);  // 归还帧缓冲

            // 自适应帧率
            vTaskDelay(pdMS_TO_TICKS(current_fps_delay));
        }

        // 推流结束，清理资源
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
        is_video_recording = false;

        // 恢复高分辨率 + 高画质拍照模式
        s->set_framesize(s, FRAMESIZE_UXGA);
        s->set_quality(s, 12);  // 恢复高质量拍照
        ESP_LOGI(TAG, "⏹️ 视频推流已结束");
    }
}

/* =====================================================================
 * 拍照并保存到 PZ 文件夹
 * =====================================================================
 * @brief 拍摄一张照片并保存到 SD 卡的 /sdcard/PZ/ 目录
 *
 * 文件命名规则: IMG_001.jpg, IMG_002.jpg, ...
 * 自动递增编号，避免覆盖已有文件 (最大 999 张)
 *
 * 流程：
 *   1. 从摄像头获取一帧 JPEG 图像
 *   2. 创建 PZ 文件夹 (如果不存在)
 *   3. 找到最小的可用编号
 *   4. 写入 SD 卡
 *   5. 释放帧缓冲
 */
void take_photo_to_PZ_folder(void) {
    /* ---- 步骤1: 获取一帧图像 ---- */
    camera_fb_t *pic = esp_camera_fb_get();
    if (!pic) {
        ESP_LOGE(TAG, "拍照失败：无法获取帧");
        return;
    }

    /* ---- 步骤2: 确保 PZ 文件夹存在 ---- */
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/PZ", MOUNT_POINT);
    mkdir(dir_path, 0777);  // 如果已存在则忽略

    /* ---- 步骤3: 找到最小的可用文件编号 ---- */
    // 核心改进：通过循环检查文件是否存在，找到最小的可用编号
    // 这样即使中间有文件被删除，也不会浪费编号
    int count = 1;
    char file_path[128];
    FILE *file = NULL;

    while (count < 999) {  // 限制最大 999 张，防止死循环
        snprintf(file_path, sizeof(file_path), "%s/IMG_%03d.jpg", dir_path, count);

        // 尝试以只读模式打开，如果成功说明文件已存在
        file = fopen(file_path, "rb");
        if (file != NULL) {
            fclose(file);  // 已存在，关闭它并尝试下一个编号
            count++;
        } else {
            // 打开失败，说明该编号可用，跳出循环
            break;
        }
    }

    /* ---- 步骤4: 写入 SD 卡 ---- */
    file = fopen(file_path, "wb");
    if (file != NULL) {
        fwrite(pic->buf, 1, pic->len, file);
        fclose(file);
        ESP_LOGI(TAG, "📸 照片已保存: %s (%d bytes)", file_path, pic->len);
    } else {
        ESP_LOGE(TAG, "❌ 无法创建文件: %s", file_path);
    }

    /* ---- 步骤5: 上传到服务器 ---- */
    upload_photo_to_server(pic);

    /* ---- 步骤6: 释放帧缓冲 ---- */
    esp_camera_fb_return(pic);  // 重要：归还给驱动，否则内存泄漏
}
