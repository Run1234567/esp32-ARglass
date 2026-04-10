#include <stdio.h>
#include <string.h>
#include "sd_card_app.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "driver/gpio.h" 
#include "freertos/FreeRTOS.h" // ? 必须加，用于 vTaskDelay
#include "freertos/task.h"

static const char *TAG = "SD_CARD";

// XIAO ESP32S3 扩展板的 SD 卡 SPI 引脚
#define PIN_NUM_MISO 8
#define PIN_NUM_MOSI 9
#define PIN_NUM_CLK  7
#define PIN_NUM_CS   21

esp_err_t init_sd_card(void) {
    esp_err_t ret;

    // 1. 配置文件系统挂载参数
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, 
        .max_files = 5,                  
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT; 

    ESP_LOGI(TAG, "正在初始化 SPI 总线...");

    // ==========================================
    // ?? 核心防御 1：强制开启数据引脚内部上拉
    // ==========================================
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK,  GPIO_PULLUP_ONLY);

    // ==========================================
    // ?? 核心防御 2：强制唤醒 SD 卡进入 SPI 模式！
    // 必须在初始化 SPI 之前，确保 CS 引脚处于稳定的高电平
    // ==========================================
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_CS),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_NUM_CS, 1);  // 强行拉高 CS 引脚
    vTaskDelay(pdMS_TO_TICKS(10));  // 停顿 10ms，让 SD 卡反应过来

    // 2. 配置 SPI 总线
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { 
        ESP_LOGE(TAG, "SPI 总线初始化失败！错误码: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. 配置 SD 卡片选引脚及驱动
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI2_HOST;

    // 4. 正式挂载虚拟文件系统
    ESP_LOGI(TAG, "正在挂载文件系统到 %s ...", mount_point);
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST; 
    
    // ==========================================
    // ?? 核心防御 3：提升主频，防止握手掉线
    // 从 400 改为 4000 (4MHz)
    // ==========================================
    host.max_freq_khz = 400; 
    
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "无法挂载文件系统。硬件可能接触不良，或卡需格式化为 FAT32。");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "读取 SD 卡超时！请拔插卡槽或检查引脚定义。");
        } else {
            ESP_LOGE(TAG, "挂载失败，底层错误码: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "? SD 卡挂载成功！");
    sdmmc_card_print_info(stdout, card);
    
    return ESP_OK;
}

void test_sd_card_read_write(void) {
    const char *file_path = MOUNT_POINT"/test.txt";
    ESP_LOGI(TAG, "--- 开始读写测试 ---");

    // ==========================================
    // 写入测试 (Write)
    // ==========================================
    ESP_LOGI(TAG, "正在创建并写入文件: %s", file_path);
    FILE *f = fopen(file_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "打开文件用于写入失败！");
        return;
    }
    fprintf(f, "Hello Seeed XIAO ESP32S3! SD Card is fully operational.\n");
    fclose(f);
    ESP_LOGI(TAG, "? 文件写入成功！");

    // ==========================================
    // 读取测试 (Read)
    // ==========================================
    ESP_LOGI(TAG, "正在读取文件...");
    f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "打开文件用于读取失败！");
        return;
    }
    
    char line[128];
    // 读取第一行
    if (fgets(line, sizeof(line), f) != NULL) {
        // 去除末尾的换行符方便打印
        char *pos = strchr(line, '\n');
        if (pos) { *pos = '\0'; }
        
        ESP_LOGI(TAG, "? 成功读取内容: '%s'", line);
    } else {
        ESP_LOGE(TAG, "文件读取为空或出错！");
    }
    fclose(f);
}