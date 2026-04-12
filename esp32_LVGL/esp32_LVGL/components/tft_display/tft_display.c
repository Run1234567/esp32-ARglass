#include "tft_display.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "TFT_DISPLAY";

// ==================== 屏幕引脚与参数配置 ====================
// 请根据你的实际接线修改这里的引脚号
#define LCD_HOST       SPI2_HOST
#define PIN_NUM_SCLK   12  // SPI 时钟引脚
#define PIN_NUM_MOSI   11  // SPI 数据引脚 (SDA)
#define PIN_NUM_MISO   -1  // 屏幕通常不需要 MISO，设为 -1
#define PIN_NUM_CS     10  // 片选引脚
#define PIN_NUM_DC     9   // 数据/命令控制引脚 (RS/DC)
#define PIN_NUM_RST    8   // 复位引脚 (RES)

#define LCD_H_RES      240 // 屏幕水平分辨率
#define LCD_V_RES      240 // 屏幕垂直分辨率

// ============================================================
// ? 全局句柄：绝对不能加 static，因为 main.c 里的 LVGL 需要拿去用！
// ============================================================
esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;

// 初始化屏幕
void lcd_init(void)
{
    ESP_LOGI(TAG, "初始化 SPI 总线...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t), // 传输缓冲区大小
    };
    // 初始化 SPI2 
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "配置 LCD 的 SPI IO 面板...");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 40 * 1000 * 1000,     // SPI 时钟频率 40MHz
        .spi_mode = 0,                   // SPI 模式 0
        .lcd_cmd_bits = 8,               // 屏幕命令是 8 bit
        .lcd_param_bits = 8,             // 屏幕参数是 8 bit
        .trans_queue_depth = 10,         // SPI 传输队列深度
        .on_color_trans_done = NULL,     // LVGL 移植包接管后，不需要我们手写回调
        .user_ctx = NULL,
    };
    // 将 IO 句柄挂载到 SPI 总线
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "安装 ST7789 驱动并初始化面板...");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, // 像素排布顺序，如果颜色颠倒可以改成 BGR
        .bits_per_pixel = 16,                       // 16bit 色深 (RGB565)
    };
    // 根据具体芯片型号安装驱动（以 ST7789 为例，如果你的屏幕是其他型号，请替换这里的函数）
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));    // 复位屏幕
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));     // 初始化屏幕指令

    // 很多 IPS 屏幕需要开启颜色反转，如果你的屏幕显示颜色是反的（比如黑色变白色），请注释掉或开启这行
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true)); 

    // 如果屏幕显示画面反了，可以取消注释下面这两行来旋转屏幕
    // ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false)); // 镜像翻转
    // ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));       // 交换 xy 轴

    ESP_LOGI(TAG, "开启屏幕显示...");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

// 绘制纯色块（主要用于前期测试，LVGL 接管后这个函数基本用不上了）
void lcd_draw_color_block(int x_start, int y_start, int x_end, int y_end, uint16_t color)
{
    int width = x_end - x_start;
    int height = y_end - y_start;
    int pixels = width * height;

    // 动态分配颜色缓冲区
    uint16_t *buffer = (uint16_t *)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "内存不足，无法分配颜色缓冲区");
        return;
    }

    // 填充颜色
    for (int i = 0; i < pixels; i++) {
        // 由于 SPI 传输涉及到大小端问题，通常需要将颜色高低位字节对调
        buffer[i] = (color >> 8) | (color << 8); 
    }

    // 刷入屏幕
    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, buffer);
    
    // 释放内存
    free(buffer);
}