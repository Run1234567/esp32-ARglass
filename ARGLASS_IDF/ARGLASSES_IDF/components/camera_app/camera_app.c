/**
 * @file camera_app.c
 * @brief 摄像头初始化与拍照模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 管理 OV2640 并行接口摄像头，提供初始化、拍照、保存到 SD 卡等功能。
 * 摄像头工作在 JPEG/UXGA (1600x1200) 模式，图像直接由硬件编码为 JPEG，
 * 帧缓冲存储在 PSRAM 中 (2 个缓冲区，双缓冲)。
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

static const char *TAG = "CAMERA_APP";  // 日志标签

/**
 * @brief 拍照状态标志
 * true = 正在拍照中 (其他模块可以据此避免冲突)
 * 由 execute_high_res_capture() 设置/清除
 */
volatile bool is_capturing = false;

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
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY; // 缓存空时自动采集
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
    // 现在 file_path 就是一个绝对不会被覆盖的路径
    file = fopen(file_path, "wb");
    if (file != NULL) {
        fwrite(pic->buf, 1, pic->len, file);  // 写入 JPEG 二进制数据
        fclose(file);
        ESP_LOGI(TAG, "📸 照片已成功保存: %s (%d bytes)", file_path, pic->len);
    } else {
        ESP_LOGE(TAG, "❌ 无法创建文件: %s", file_path);
    }

    /* ---- 步骤5: 释放帧缓冲 ---- */
    esp_camera_fb_return(pic);  // 重要：归还给驱动，否则内存泄漏
}
