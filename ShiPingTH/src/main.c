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

#define WIFI_SSID      "RUN"
#define WIFI_PASS      "88888888"

static const char *TAG = "CAMERA_STREAM";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

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

const char* index_html =
"<!DOCTYPE html><html><head><title>ESP32-S3 Camera Stream</title>"
"<style>body{background:#222;color:#fff;text-align:center;font-family:sans-serif;}"
"img{max-width:100%;border:2px solid #555;}</style></head>"
"<body><h2>ESP32-S3 Camera Stream</h2>"
"<img id='stream' src='/stream' />"
"<p id='fps'>Connecting...</p>"
"<script>"
"let f=0;"
"setInterval(()=>{document.getElementById('fps').innerText='FPS: '+f;f=0;},1000);"
"const img=document.getElementById('stream');"
"img.onload=()=>{f++;};"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    ESP_LOGI(TAG, "Client connected to stream");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            esp_camera_fb_return(fb);
            fb = NULL;
            if (!jpeg_converted) {
                ESP_LOGE(TAG, "JPEG compression failed");
                res = ESP_FAIL;
                break;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }

        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }

        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Stream send failed or client disconnected");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Stream closed");
    return res;
}

void start_camera_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &stream_uri);
    }
}

#define WS2812_PIN 48
#define WS2812_NUM 1

static led_strip_handle_t configure_led(void)
{
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

static void set_rainbow_color(led_strip_handle_t strip, uint8_t pos)
{
    uint8_t r = 0, g = 0, b = 0;
    if (pos < 85) {
        r = pos * 3;
        g = 255 - pos * 3;
        b = 0;
    } else if (pos < 170) {
        pos -= 85;
        r = 255 - pos * 3;
        g = 0;
        b = pos * 3;
    } else {
        pos -= 170;
        r = 0;
        g = pos * 3;
        b = 255 - pos * 3;
    }

    r /= 10;
    g /= 10;
    b /= 10;

    led_strip_set_pixel(strip, 0, g, r, b);
}

static void rainbow_task(void *arg)
{
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

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Waiting for IP address...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "Connected! IP Address: " IPSTR, IP2STR(&ip_info.ip));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "初始化 WS2812...");
    led_strip_handle_t led_strip = configure_led();
    led_strip_clear(led_strip);
    xTaskCreate(rainbow_task, "rainbow", 2048, (void *)led_strip, 5, NULL);

    ESP_LOGI(TAG, "正在初始化摄像头...");

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: 0x%x", err);
        ESP_LOGE(TAG, "请检查摄像头连接和引脚配置");
        return;
    }
    ESP_LOGI(TAG, "摄像头初始化成功！");

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "传感器 PID: 0x%02X", s->id.PID);
        ESP_LOGI(TAG, "传感器 VER: 0x%02X", s->id.VER);
        s->set_vflip(s, 0);
        s->set_hmirror(s, 0);
    }

    wifi_init_sta();

    start_camera_server();

    ESP_LOGI(TAG, "Camera Stream Server is ready.");
    ESP_LOGI(TAG, "Open http://<IP>/stream in your browser.");
}
