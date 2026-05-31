#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

// ================= 配置区 =================
#define WIFI_SSID       "RUN"
#define WIFI_PASS       "88888888"

#define I2S_SCK_PIN    41
#define I2S_WS_PIN     42
#define I2S_SD_PIN     2
#define SAMPLE_RATE    16000

#define SPK_BCLK_PIN   39
#define SPK_LRC_PIN    40
#define SPK_DIN_PIN    38

#define WS2812_PIN     48
#define WS2812_NUM     1

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

#define TFT_SCK_PIN    20
#define TFT_MOSI_PIN   21
#define TFT_CS_PIN     19
#define TFT_DC_PIN     47
#define TFT_RST_PIN    14
#define TFT_BLK_PIN    1

#define TFT_WIDTH      128
#define TFT_HEIGHT     160

static const char *TAG = "AV_STREAM";

i2s_chan_handle_t rx_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

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

// ================= 1. WS2812 LED =================
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
    r /= 10; g /= 10; b /= 10;
    led_strip_set_pixel(strip, 0, g, r, b);
}

static void rainbow_task(void *arg) {
    led_strip_handle_t led_strip = (led_strip_handle_t)arg;
    uint8_t color_pos = 0;
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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_SCK_PIN, .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED, .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S 麦克风初始化成功！");
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

// ================= 3. HTTP 服务器 =================
const char* index_html =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 视听双流监控</title>"
"<style>body{background:#222;color:#fff;text-align:center;font-family:sans-serif;margin-top:20px;}"
"img{max-width:100%;border:2px solid #555;margin-bottom:20px;}</style></head>"
"<body><h2>ESP32-S3 视听双流监听器</h2>"
"<img id='stream' src='/stream' /><br>"
"<p id='fps'>Connecting...</p>"
"<p style='color:yellow;'>&#x26A0; 如果听不到声音，请确保点击下方的播放按钮</p>"
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

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

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
    return res;
}

static esp_err_t audio_stream_handler(httpd_req_t *req) {
    esp_err_t res = ESP_OK;
    size_t bytes_read = 0;

    #define MIC_BUF_SAMPLES 512
    int32_t *i2s_read_buff = (int32_t *)malloc(MIC_BUF_SAMPLES * sizeof(int32_t));
    int16_t *wav_buff = (int16_t *)malloc(MIC_BUF_SAMPLES * sizeof(int16_t));

    if (!i2s_read_buff || !wav_buff) {
        ESP_LOGE(TAG, "音频流内存不足！");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "audio/wav");
    ESP_LOGI(TAG, "Client connected to Audio stream...");

    uint8_t wav_header[44];
    generate_wav_header(wav_header, SAMPLE_RATE, 16, 1);
    res = httpd_resp_send_chunk(req, (const char *)wav_header, 44);

    if (res == ESP_OK) {
        while (1) {
            res = i2s_channel_read(rx_handle, i2s_read_buff, MIC_BUF_SAMPLES * sizeof(int32_t), &bytes_read, portMAX_DELAY);

            if (res == ESP_OK && bytes_read > 0) {
                int frames_read = bytes_read / (2 * sizeof(int32_t));

                for (int i = 0; i < frames_read; i++) {
                    int32_t sample = i2s_read_buff[i * 2] >> 16;

                    sample = sample * 2;
                    if (sample > 32767)  sample = 32767;
                    if (sample < -32768) sample = -32768;
                    wav_buff[i] = (int16_t)sample;
                }

                res = httpd_resp_send_chunk(req, (const char *)wav_buff, frames_read * sizeof(int16_t));
                if (res != ESP_OK) break;
            }
        }
    }

    free(i2s_read_buff);
    free(wav_buff);

    ESP_LOGI(TAG, "Audio stream closed.");
    return res;
}

void start_webserver() {
    httpd_config_t config_video = HTTPD_DEFAULT_CONFIG();
    config_video.server_port = 80;
    config_video.ctrl_port = 32768;

    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

    httpd_handle_t server_video = NULL;
    if (httpd_start(&server_video, &config_video) == ESP_OK) {
        httpd_register_uri_handler(server_video, &index_uri);
        httpd_register_uri_handler(server_video, &stream_uri);
    }

    httpd_config_t config_audio = HTTPD_DEFAULT_CONFIG();
    config_audio.server_port = 81;
    config_audio.ctrl_port = 32769;

    httpd_uri_t audio_uri = { .uri = "/audio", .method = HTTP_GET, .handler = audio_stream_handler, .user_ctx = NULL };

    httpd_handle_t server_audio = NULL;
    if (httpd_start(&server_audio, &config_audio) == ESP_OK) {
        httpd_register_uri_handler(server_audio, &audio_uri);
    }
}

// ================= 4. 扬声器 I2S 初始化 =================
void init_speaker() {
    ESP_LOGI(TAG, "正在使用全新配置初始化 MAX98357 扬声器...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRC_PIN,
            .dout = SPK_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

// ================= 实时扩音器任务 (Mic -> Speaker) =================
static void megaphone_task(void *arg) {
    size_t bytes_read = 0;
    size_t bytes_written = 0;

    #define CHUNK_SAMPLES 512

    int32_t *mic_buff = (int32_t *)malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t));
    int16_t *spk_buff = (int16_t *)malloc(CHUNK_SAMPLES * sizeof(int16_t));

    if (!mic_buff || !spk_buff) {
        ESP_LOGE(TAG, "扩音器内存分配失败！");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "实时扩音器已启动！请对着麦克风说话...");

    while (1) {
        if (i2s_channel_read(rx_handle, mic_buff, CHUNK_SAMPLES * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {

            int frames = bytes_read / (2 * sizeof(int32_t));

            for (int i = 0; i < frames; i++) {
                int32_t sample = mic_buff[i * 2] >> 16;

                sample = sample * 4;

                if (sample > 32767)  sample = 32767;
                if (sample < -32768) sample = -32768;

                spk_buff[i] = (int16_t)sample;
            }

            i2s_channel_write(tx_handle, spk_buff, frames * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
    }

    free(mic_buff);
    free(spk_buff);
    vTaskDelete(NULL);
}

void stop_speaker() {
    if (tx_handle != NULL) {
        ESP_LOGI(TAG, "正在关闭扬声器并强制切断物理噪声...");

        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;

        gpio_reset_pin(SPK_BCLK_PIN);
        gpio_reset_pin(SPK_LRC_PIN);
        gpio_reset_pin(SPK_DIN_PIN);

        gpio_set_direction(SPK_BCLK_PIN, GPIO_MODE_OUTPUT);
        gpio_set_direction(SPK_LRC_PIN, GPIO_MODE_OUTPUT);
        gpio_set_direction(SPK_DIN_PIN, GPIO_MODE_OUTPUT);

        gpio_set_level(SPK_BCLK_PIN, 0);
        gpio_set_level(SPK_LRC_PIN, 0);
        gpio_set_level(SPK_DIN_PIN, 0);

        ESP_LOGI(TAG, "扬声器总线已完全释放且引脚已强行拉低。");
    }
}

// ================= 屏幕初始化 (ST7735) =================
void init_tft() {
    ESP_LOGI(TAG, "初始化 SPI 总线...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SCK_PIN,
        .mosi_io_num = TFT_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 16 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "配置 LCD 面板 IO...");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_DC_PIN,
        .cs_gpio_num = TFT_CS_PIN,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "安装原生 ST7789 面板驱动 (向下兼容 ST7735)...");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    esp_lcd_panel_set_gap(panel_handle, 0, 0);

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "点亮背光...");
    gpio_reset_pin(TFT_BLK_PIN);
    gpio_set_direction(TFT_BLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_BLK_PIN, 1);
}

// ================= LVGL 对接回调 =================
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t) drv->user_data;
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, color_map);
}

static void lvgl_tick_task(void *arg) {
    lv_tick_inc(2);
}

void init_lvgl(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io) {
    ESP_LOGI(TAG, "初始化 LVGL 核心...");
    lv_init();

    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };

    #define LVGL_BUFFER_SIZE (TFT_WIDTH * TFT_HEIGHT / 10)
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, LVGL_BUFFER_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TFT_WIDTH;
    disp_drv.ver_res = TFT_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel;

    esp_lcd_panel_io_register_event_callbacks(io, &cbs, &disp_drv);

    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_task,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2 * 1000));
}

// ================= 极简 8x16 像素 ASCII 字库 =================
const unsigned char font_8x16[][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x10,0x28,0x28,0x28,0x44,0x44,0x7C,0x82,0x82,0x82,0x00,0x00,0x00},
    {0x00,0x00,0x00,0xFC,0x42,0x42,0x42,0x7C,0x42,0x42,0x42,0x42,0xFC,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x3C,0x42,0x80,0x80,0x80,0x80,0x80,0x80,0x42,0x3C,0x00,0x00,0x00},
    {0x00,0x00,0x00,0xF8,0x44,0x42,0x42,0x42,0x42,0x42,0x42,0x44,0xF8,0x00,0x00,0x00},
    {0x00,0x00,0x00,0xFC,0x40,0x40,0x40,0x78,0x40,0x40,0x40,0x40,0xFC,0x00,0x00,0x00},
};

int get_font_index(char c) {
    if (c == ' ') return 0;
    if (c >= 'A' && c <= 'E') return c - 'A' + 1;
    return 0;
}

void draw_char(int x, int y, char c, uint16_t fg_color, uint16_t bg_color) {
    uint16_t *char_buf = (uint16_t *)heap_caps_malloc(8 * 16 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!char_buf) return;

    int font_idx = get_font_index(c);

    for (int row = 0; row < 16; row++) {
        unsigned char row_data = font_8x16[font_idx][row];
        for (int col = 0; col < 8; col++) {
            if (row_data & (0x80 >> col)) {
                char_buf[row * 8 + col] = fg_color;
            } else {
                char_buf[row * 8 + col] = bg_color;
            }
        }
    }

    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 8, y + 16, char_buf);

    free(char_buf);
}

void draw_string(int start_x, int start_y, const char *str, uint16_t fg_color, uint16_t bg_color) {
    int x = start_x;
    int y = start_y;
    while (*str) {
        draw_char(x, y, *str, fg_color, bg_color);
        x += 8;
        if (x + 8 > TFT_WIDTH) {
            x = start_x;
            y += 16;
        }
        str++;
    }
}

// ================= 5. WiFi 初始化 =================
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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_strip_handle_t led_strip = configure_led();
    led_strip_clear(led_strip);
    xTaskCreate(rainbow_task, "rainbow", 2048, (void *)led_strip, 5, NULL);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) { s->set_vflip(s, 0); s->set_hmirror(s, 0); }
    }

    init_tft();

    init_lvgl(panel_handle, io_handle);

    ESP_LOGI(TAG, "创建 LVGL 界面...");
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00A8FF), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "J.A.R.V.I.S.");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 100, 10);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 30);
    lv_bar_set_value(bar, 75, LV_ANIM_ON);

    init_microphone();
    init_speaker();

    xTaskCreate(megaphone_task, "megaphone", 8192, NULL, 5, NULL);

    wifi_init_sta();
    start_webserver();

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
