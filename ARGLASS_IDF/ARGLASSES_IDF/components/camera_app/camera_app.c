#include "camera_app.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "my_uart.h"
#include "driver/uart.h"
#include <stdio.h>
#include <sys/stat.h>
#include "sd_card_app.h"

static const char *TAG = "CAMERA_APP";

volatile bool is_capturing = false;

void execute_high_res_capture(void) {
    is_capturing = true;
    vTaskDelay(pdMS_TO_TICKS(50));

    sensor_t *s = esp_camera_sensor_get();
    s->set_pixformat(s, PIXFORMAT_JPEG);
    s->set_framesize(s, FRAMESIZE_UXGA);
    s->set_quality(s, 12);
    vTaskDelay(pdMS_TO_TICKS(500));

    for (int i = 0; i < 2; i++) {
        camera_fb_t *dummy_fb = esp_camera_fb_get();
        if (dummy_fb) esp_camera_fb_return(dummy_fb);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    extern void take_photo_to_PZ_folder(void);
    take_photo_to_PZ_folder();

    s->set_framesize(s, FRAMESIZE_UXGA);
    s->set_pixformat(s, PIXFORMAT_JPEG);

    is_capturing = false;
    my_uart_send("CMD:PHOTO_DONE\r\n");
}

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11 
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

void initCamera(void) {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;

    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    config.jpeg_quality = 12;

    ESP_ERROR_CHECK(esp_camera_init(&config));

    ESP_LOGI(TAG, "摄像头初始化完成 (JPEG 模式，仅拍照功能)");
    
    for (int i = 0; i < 3; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "摄像头预热完成");
}

void take_photo_to_PZ_folder(void) {
    camera_fb_t *pic = esp_camera_fb_get();
    if (!pic) {
        ESP_LOGE(TAG, "拍照失败：无法获取帧");
        return;
    }

    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/PZ", MOUNT_POINT);
    mkdir(dir_path, 0777);

    // 核心改进：通过循环检查文件是否存在，找到最小的可用编号
    int count = 1;
    char file_path[128];
    FILE *file = NULL;

    while (count < 999) { // 限制最大 999 张，防止死循环
        snprintf(file_path, sizeof(file_path), "%s/IMG_%03d.jpg", dir_path, count);
        
        // 尝试以只读模式打开，如果成功说明文件已存在
        file = fopen(file_path, "rb");
        if (file != NULL) {
            fclose(file); // 已存在，关闭它并寻找下一个编号
            count++;
        } else {
            // 打开失败，说明该编号可用，跳出循环
            break; 
        }
    }

    // 现在 file_path 就是一个绝对不会被覆盖的路径
    file = fopen(file_path, "wb");
    if (file != NULL) {
        fwrite(pic->buf, 1, pic->len, file);
        fclose(file);
        ESP_LOGI(TAG, "照片已成功保存: %s (%d bytes)", file_path, pic->len);
    } else {
        ESP_LOGE(TAG, "无法创建文件: %s", file_path);
    }
    
    esp_camera_fb_return(pic);
}