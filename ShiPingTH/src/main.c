#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/gpio_reg.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "ov7725_regs.h"

static const char *TAG = "OV7725_FIFO";

#define WIFI_SSID "RUN"
#define WIFI_PASS "88888888"

#define PIN_SCL     1
#define PIN_SDA     2
#define PIN_VSYNC   8
#define PIN_WEN     9
#define PIN_WRST    10
#define PIN_RRST    11
#define PIN_RCLK    12

#define IMG_WIDTH   320
#define IMG_HEIGHT  240
#define FRAME_SIZE  (IMG_WIDTH * IMG_HEIGHT * 2)

#define I2C_MASTER_NUM   I2C_NUM_0

uint8_t *frame_buffer = NULL;
SemaphoreHandle_t vsync_semaphore = NULL;

const char* index_html =
"<!DOCTYPE html><html><head><title>ESP32-S3 摄像头实时图传</title>"
"<style>body{background:#222;color:#fff;text-align:center;font-family:sans-serif;}"
"canvas{background:#000;border:2px solid #555;width:640px;height:480px;image-rendering:pixelated;}</style></head>"
"<body><h2>ESP32-S3 Raw Stream</h2><canvas id='cam' width='320' height='240'></canvas><br><br>"
"<button onclick='s=!s;this.innerText=s?\"模式：小端 (已切换)\":\"模式：大端 (默认)\"'>修复偏色 (Swap Bytes)</button>"
"<p id='fps'>FPS: 0</p>"
"<script>const ctx=document.getElementById('cam').getContext('2d');"
"const imgData=ctx.createImageData(320,240);let s=true,f=0;"
"setInterval(()=>{document.getElementById('fps').innerText='FPS: '+f;f=0;},1000);"
"async function st(){try{let r=await fetch('/frame');if(r.ok){"
"let b=await r.arrayBuffer(),a=new Uint8Array(b);"
"for(let i=0,j=0;i<153600;i+=2,j+=4){"
"let p=s?((a[i+1]<<8)|a[i]):((a[i]<<8)|a[i+1]);"
"imgData.data[j]=(p>>8)&0xF8;imgData.data[j+1]=(p>>3)&0xFC;"
"imgData.data[j+2]=(p<<3)&0xF8;imgData.data[j+3]=255;}"
"ctx.putImageData(imgData,0,0);f++;}}catch(e){}"
"requestAnimationFrame(st);}st();</script></body></html>";

static void IRAM_ATTR vsync_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(vsync_semaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static inline uint8_t read_fifo_byte_fast(void)
{
    REG_WRITE(GPIO_OUT_W1TS_REG, (1 << PIN_RCLK));
    __asm__ __volatile__("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");

    uint32_t in_val = REG_READ(GPIO_IN_REG);

    REG_WRITE(GPIO_OUT_W1TC_REG, (1 << PIN_RCLK));
    __asm__ __volatile__("nop\nnop\nnop\nnop");

    uint8_t data = ((in_val >> 4) & 0x0F) | ((in_val >> 11) & 0xF0);

    return data;
}

void init_camera_gpios(void)
{
    gpio_config_t io_conf = {};

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_WEN) | (1ULL << PIN_WRST) |
                           (1ULL << PIN_RRST) | (1ULL << PIN_RCLK);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    gpio_set_level(PIN_WEN, 0);
    gpio_set_level(PIN_WRST, 1);
    gpio_set_level(PIN_RRST, 1);
    gpio_set_level(PIN_RCLK, 0);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) |
                           (1ULL << 15) | (1ULL << 16) | (1ULL << 17) | (1ULL << 18);
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.pin_bit_mask = (1ULL << PIN_VSYNC);
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_VSYNC, vsync_isr_handler, NULL);
}

esp_err_t sccb_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OV7725_SCCB_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t sccb_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OV7725_SCCB_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;

    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OV7725_SCCB_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, val, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

void init_sccb(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PIN_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

esp_err_t ov7725_init(void)
{
    uint8_t pid = 0, ver = 0;
    esp_err_t ret;

    ret = sccb_read_reg(OV7725_PID, &pid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 PID 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = sccb_read_reg(OV7725_VER, &ver);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 VER 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "OV7725 PID=0x%02X, VER=0x%02X", pid, ver);

    if (pid != OV7725_PID_VALUE) {
        ESP_LOGW(TAG, "PID 不匹配，期望 0x%02X，实际 0x%02X", OV7725_PID_VALUE, pid);
    }

    for (int i = 0; i < OV7725_QVGA_RGB565_SIZE; i++) {
        ret = sccb_write_reg(ov7725_qvga_rgb565[i].reg, ov7725_qvga_rgb565[i].val);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "写入寄存器 0x%02X 失败", ov7725_qvga_rgb565[i].reg);
            return ret;
        }
    }

    ESP_LOGI(TAG, "OV7725 QVGA RGB565 配置完成");
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t frame_handler(httpd_req_t *req)
{
    ESP_LOGI("FRAME", "收到帧请求，开始采集...");

    esp_err_t sem_ret = xSemaphoreTake(vsync_semaphore, pdMS_TO_TICKS(2000));
    if (sem_ret != pdTRUE) {
        ESP_LOGE("FRAME", "等待 VSYNC 超时！检查摄像头连接");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "VSYNC timeout", 14);
    }
    ESP_LOGI("FRAME", "VSYNC 信号收到，开始写入 FIFO");

    gpio_set_level(PIN_WRST, 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_WRST, 1);

    gpio_set_level(PIN_WEN, 1);

    sem_ret = xSemaphoreTake(vsync_semaphore, pdMS_TO_TICKS(2000));
    if (sem_ret != pdTRUE) {
        ESP_LOGE("FRAME", "等待第二 VSYNC 超时！");
        gpio_set_level(PIN_WEN, 0);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "VSYNC2 timeout", 15);
    }
    gpio_set_level(PIN_WEN, 0);
    ESP_LOGI("FRAME", "FIFO 写入完成，开始读取");

    gpio_set_level(PIN_RRST, 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_RRST, 1);

    uint32_t ptr = 0;
    for (uint32_t i = 0; i < FRAME_SIZE; i++) {
        frame_buffer[ptr++] = read_fifo_byte_fast();
    }

    ESP_LOGI("FRAME", "读取完成，前16字节:");
    ESP_LOGI("FRAME", "%02X %02X %02X %02X %02X %02X %02X %02X",
             frame_buffer[0], frame_buffer[1], frame_buffer[2], frame_buffer[3],
             frame_buffer[4], frame_buffer[5], frame_buffer[6], frame_buffer[7]);
    ESP_LOGI("FRAME", "%02X %02X %02X %02X %02X %02X %02X %02X",
             frame_buffer[8], frame_buffer[9], frame_buffer[10], frame_buffer[11],
             frame_buffer[12], frame_buffer[13], frame_buffer[14], frame_buffer[15]);

    uint32_t non_zero = 0;
    uint32_t unique_count = 0;
    for (uint32_t i = 0; i < FRAME_SIZE; i++) {
        if (frame_buffer[i] != 0) non_zero++;
    }
    for (uint32_t i = 2; i < 100; i++) {
        if (frame_buffer[i] != frame_buffer[i-2]) unique_count++;
    }
    ESP_LOGI("FRAME", "非零字节数: %lu / %lu", non_zero, FRAME_SIZE);
    ESP_LOGI("FRAME", "前100字节中不同值的像素对数: %lu", unique_count);

    for (int i = 0; i < 4; i++) {
        uint16_t pixel = (frame_buffer[i*2] << 8) | frame_buffer[i*2+1];
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        ESP_LOGI("FRAME", "像素%d: 0x%04X -> R=%d G=%d B=%d", i, pixel, r, g, b);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)frame_buffer, FRAME_SIZE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "系统启动，准备点亮 AR 眼镜的视野...");

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    vsync_semaphore = xSemaphoreCreateBinary();

    frame_buffer = (uint8_t *)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM);
    if (frame_buffer == NULL) {
        ESP_LOGE(TAG, "PSRAM 内存分配失败！");
        ESP_LOGE(TAG, "请确认: 1) 板载有 PSRAM  2) sdkconfig 已启用 CONFIG_SPIRAM=y");
        return;
    }
    ESP_LOGI(TAG, "PSRAM 显存分配成功: %d Bytes", FRAME_SIZE);

    init_sccb();
    init_camera_gpios();

    esp_err_t ret = ov7725_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OV7725 初始化失败！");
        return;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "正在连接 Wi-Fi... 请注意观察串口打印的 IP 地址！");

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 8;
    server_config.core_id = 1;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_config) == ESP_OK) {
        httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_uri_t uri_frame = { .uri = "/frame", .method = HTTP_GET, .handler = frame_handler };
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_frame);
        ESP_LOGI(TAG, "Web 服务器已启动！");
    }

    ESP_LOGI(TAG, "图像分辨率: %dx%d, 帧大小: %d Bytes (RGB565)", IMG_WIDTH, IMG_HEIGHT, FRAME_SIZE);
}
