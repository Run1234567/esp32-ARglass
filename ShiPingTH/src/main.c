#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "led_strip.h"
#include "driver/i2s_std.h"

// ================= 配置区 =================
#define WIFI_SSID      "RUN"      // 替换为你的 WiFi 名称
#define WIFI_PASS      "88888888" // 替换为你的 WiFi 密码

// --- 麦克风引脚配置 (INMP441) ---
#define I2S_SCK_PIN    41  // SCK / BCLK
#define I2S_WS_PIN     42  // WS / LRCK
#define I2S_SD_PIN     2   // SD / DATA IN
#define SAMPLE_RATE    16000 // 16kHz 采样率

// --- WS2812 LED 配置 ---
#define WS2812_PIN     48
#define WS2812_NUM     1

// --- 摄像头引脚配置 ---
#define PIN_CAM_XCLK   15
#define PIN_CAM_SIOD   4
#define PIN_CAM_SIOC   5
#define PIN_CAM_Y9     16
#define PIN_CAM_Y8     17
#define PIN_CAM_Y7     18
#define PIN_CAM_Y6     12
#define PIN_CAM_Y5     10
#define PIN_CAM_Y4     8
#define PIN_CAM_Y3     9
#define PIN_CAM_Y2     11
#define PIN_CAM_VSYNC  6
#define PIN_CAM_HREF   7
#define PIN_CAM_PCLK   13

static const char *TAG = "AV_STREAM";

// I2S 接收通道句柄
i2s_chan_handle_t rx_handle = NULL; 

// 视频流相关的宏
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// 摄像头配置
static camera_config_t camera_config = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = PIN_CAM_XCLK,
    .pin_sccb_sda = PIN_CAM_SIOD,
    .pin_sccb_scl = PIN_CAM_SIOC,
    .pin_d7       = PIN_CAM_Y9,
    .pin_d6       = PIN_CAM_Y8,
    .pin_d5       = PIN_CAM_Y7,
    .pin_d4       = PIN_CAM_Y6,
    .pin_d3       = PIN_CAM_Y5,
    .pin_d2       = PIN_CAM_Y4,
    .pin_d1       = PIN_CAM_Y3,
    .pin_d0       = PIN_CAM_Y2,
    .pin_vsync    = PIN_CAM_VSYNC,
    .pin_href     = PIN_CAM_HREF,
    .pin_pclk     = PIN_CAM_PCLK,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size   = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count     = 1,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY
};

// ================= 1. WS2812 LED 功能 =================
static led_strip_handle_t configure_led(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_PIN,
        .max_leds = WS2812_NUM,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    return led_strip;
}

static void set_rainbow_color(led_strip_handle_t strip, uint8_t pos) {
    uint8_t r = 0, g = 0, b = 0;
    if (pos < 85) { r = pos * 3; g = 255 - pos * 3; b = 0; } 
    else if (pos < 170) { pos -= 85; r = 255 - pos * 3; g = 0; b = pos * 3; } 
    else { pos -= 170; r = 0; g = pos * 3; b = 255 - pos * 3; }
    r /= 10; g /= 10; b /= 10; // 降低亮度
    led_strip_set_pixel(strip, 0, g, r, b);
}

static void rainbow_task(void *arg) {
    led_strip_handle_t led_strip = (led_strip_handle_t)arg;
    uint8_t color_pos = 0;
    ESP_LOGI(TAG, "彩虹渐变任务启动");
    while (1) {
        set_rainbow_color(led_strip, color_pos);
        led_strip_refresh(led_strip);
        color_pos++;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// ================= 2. 麦克风 I2S 初始化 =================
void init_microphone() {
    ESP_LOGI(TAG, "初始化 I2S 麦克风...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // 注意：这里必须是 32BIT，为了匹配 INMP441 的硬件时钟要求
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_SCK_PIN, .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED, .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S 初始化成功！");
}

void generate_wav_header(uint8_t *header, uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels) {
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint32_t file_size = 0xFFFFFFFF; 
    uint32_t data_size = 0xFFFFFFFF;
    const uint8_t wav_header[44] = {
        'R', 'I', 'F', 'F',
        (uint8_t)(file_size & 0xff), (uint8_t)((file_size >> 8) & 0xff), (uint8_t)((file_size >> 16) & 0xff), (uint8_t)((file_size >> 24) & 0xff),
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, channels, 0,
        (uint8_t)(sample_rate & 0xff), (uint8_t)((sample_rate >> 8) & 0xff), (uint8_t)((sample_rate >> 16) & 0xff), (uint8_t)((sample_rate >> 24) & 0xff),
        (uint8_t)(byte_rate & 0xff), (uint8_t)((byte_rate >> 8) & 0xff), (uint8_t)((byte_rate >> 16) & 0xff), (uint8_t)((byte_rate >> 24) & 0xff),
        (uint8_t)(channels * bits_per_sample / 8), 0, (uint8_t)(bits_per_sample), 0,
        'd', 'a', 't', 'a',
        (uint8_t)(data_size & 0xff), (uint8_t)((data_size >> 8) & 0xff), (uint8_t)((data_size >> 16) & 0xff), (uint8_t)((data_size >> 24) & 0xff)
    };
    memcpy(header, wav_header, 44);
}

// ================= 3. HTTP 服务器流处理 =================
// 综合的 HTML：同时包含图像 img 标签和声音 audio 标签
const char* index_html =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 视听双流监控</title>"
"<style>body{background:#222;color:#fff;text-align:center;font-family:sans-serif;margin-top:20px;}"
"img{max-width:100%;border:2px solid #555;margin-bottom:20px;}</style></head>"
"<body><h2>ESP32-S3 视听双流监听器</h2>"
"<img id='stream' src='/stream' /><br>"
"<p id='fps'>Connecting...</p>"
"<p style='color:yellow;'>⚠️ 如果听不到声音，请确保点击下方的播放按钮</p>"
"<audio controls autoplay id='audio_player'>您的浏览器不支持。</audio>"
"<script>"
"let f=0;"
"setInterval(()=>{document.getElementById('fps').innerText='FPS: '+f;f=0;},1000);"
"const img=document.getElementById('stream');"
"img.onload=()=>{f++;};"
"document.getElementById('audio_player').src = 'http://' + window.location.hostname + ':81/audio';"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// 视频流 Handler
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    ESP_LOGI(TAG, "Client connected to Video stream");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }

        if (fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            esp_camera_fb_return(fb);
            fb = NULL;
            if (!jpeg_converted) { res = ESP_FAIL; break; }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) { res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len); }
        if (res == ESP_OK) { res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)); }

        if (fb) { esp_camera_fb_return(fb); fb = NULL; _jpg_buf = NULL; } 
        else if (_jpg_buf) { free(_jpg_buf); _jpg_buf = NULL; }

        if (res != ESP_OK) { break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "Video Stream closed");
    return res;
}

// 音频流 Handler (已修复 32位数据提取 和 去除休眠)
static esp_err_t audio_stream_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    size_t bytes_read = 0;
    int32_t i2s_read_buff[256]; // 读取 32位 原始数据
    int16_t wav_buff[256];      // 发送 16位 声音数据

    httpd_resp_set_type(req, "audio/wav");
    ESP_LOGI(TAG, "Client connected to Audio stream...");

    uint8_t wav_header[44];
    generate_wav_header(wav_header, SAMPLE_RATE, 16, 1);
    res = httpd_resp_send_chunk(req, (const char *)wav_header, 44);
    if (res != ESP_OK) return res;

    while (1) {
        res = i2s_channel_read(rx_handle, i2s_read_buff, sizeof(i2s_read_buff), &bytes_read, portMAX_DELAY);

        if (res == ESP_OK && bytes_read > 0) {
            int samples_read = bytes_read / sizeof(int32_t);
            // 32 位转 16 位，右移 14 位提取有效声音数据
            for (int i = 0; i < samples_read; i++) {
                wav_buff[i] = i2s_read_buff[i] >> 14;
            }
            res = httpd_resp_send_chunk(req, (const char *)wav_buff, samples_read * sizeof(int16_t));
            if (res != ESP_OK) break;
        }
    }
    ESP_LOGI(TAG, "Audio stream closed");
    return res;
}

// 统一启动 Web 服务器
void start_webserver() {
    // ---- 1. 配置视频和主页服务器 (端口 80) ----
    httpd_config_t config_video = HTTPD_DEFAULT_CONFIG();
    config_video.server_port = 80;
    config_video.ctrl_port = 32768;

    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

    httpd_handle_t server_video = NULL;
    if (httpd_start(&server_video, &config_video) == ESP_OK) {
        httpd_register_uri_handler(server_video, &index_uri);
        httpd_register_uri_handler(server_video, &stream_uri);
        ESP_LOGI(TAG, "视频服务器已启动 (端口: 80)");
    }

    // ---- 2. 配置音频流服务器 (端口 81) ----
    httpd_config_t config_audio = HTTPD_DEFAULT_CONFIG();
    config_audio.server_port = 81;
    config_audio.ctrl_port = 32769;

    httpd_uri_t audio_uri = { .uri = "/audio", .method = HTTP_GET, .handler = audio_stream_handler, .user_ctx = NULL };

    httpd_handle_t server_audio = NULL;
    if (httpd_start(&server_audio, &config_audio) == ESP_OK) {
        httpd_register_uri_handler(server_audio, &audio_uri);
        ESP_LOGI(TAG, "音频独立服务器已启动 (端口: 81)");
    }
}

// ================= 4. WiFi 初始化 =================
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Waiting for IP address...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "Connected! 打开浏览器输入: http://" IPSTR, IP2STR(&ip_info.ip));
}

// ================= 主函数 =================
void app_main(void) {
    // 1. 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 WS2812 灯效
    ESP_LOGI(TAG, "初始化 WS2812...");
    led_strip_handle_t led_strip = configure_led();
    led_strip_clear(led_strip);
    xTaskCreate(rainbow_task, "rainbow", 2048, (void *)led_strip, 5, NULL);

    // 3. 初始化摄像头
    ESP_LOGI(TAG, "正在初始化摄像头...");
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: 0x%x", err);
    } else {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_vflip(s, 0);   // 根据摆放可以修改为 1
            s->set_hmirror(s, 0); // 根据摆放可以修改为 1
        }
        ESP_LOGI(TAG, "摄像头初始化成功！");
    }

    // 4. 初始化麦克风
    init_microphone();

    // 5. 连接 WiFi
    wifi_init_sta();

    // 6. 启动集成服务器
    start_webserver();
}