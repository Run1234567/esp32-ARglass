#include <stdio.h> 
#include <string.h> 
#include "esp_log.h" 
#include "esp_vfs_fat.h" 
#include "sdmmc_cmd.h" 
#include "driver/sdmmc_host.h" 
#include "driver/gpio.h" 

// 引入自己的头文件 
#include "sd_card_app.h" 
#include "tts_app.h"        // ✨ 你的发声引擎 
#include "my_uart.h"        // ✨ 引入串口模块 (彻底替换了 app_mqtt.h) 

static const char *TAG = "SD_APP" ; 

// ========================================== 
// ⚠️ 注意：send_novel_chunk_via_mqtt 函数已被彻底删除！ 
// cJSON 和 MQTT 的引用也已全部移除，代码变得极度清爽。 
// ========================================== 

esp_err_t init_sd_card(void) { 
    esp_err_t  ret; 
    sdmmc_card_t  *card; 

    esp_vfs_fat_sdmmc_mount_config_t  mount_config = { 
        .format_if_mount_failed = false , 
        .max_files = 5 , 
        .allocation_unit_size = 16 * 1024 
    }; 

    ESP_LOGI(TAG, "正在初始化原生 SDMMC 总线..." ); 

    sdmmc_host_t  host = SDMMC_HOST_DEFAULT(); 
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; 

    // 官方 Sense 扩展板引脚映射 
    sdmmc_slot_config_t  slot_config = SDMMC_SLOT_CONFIG_DEFAULT(); 
    slot_config.width = 1 ;         
    slot_config.clk = 7 ;           
    slot_config.cmd = 9 ;           
    slot_config.d0  = 8 ;           
    slot_config.d1 = -1 ; 
    slot_config.d2 = -1 ; 
    slot_config.d3 = -1 ; 
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; 

    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card); 

    if  (ret != ESP_OK) { 
        ESP_LOGE(TAG, "挂载失败，错误码: %s" , esp_err_to_name(ret)); 
        return  ret; 
    } 

    ESP_LOGI(TAG, "🌟 模块化原生 SDMMC 挂载成功！" ); 
    sdmmc_card_print_info(stdout , card); 
    
    return  ESP_OK; 
} 

void test_sd_card_read_write(void) { 
    const char *file_path = MOUNT_POINT"/test.txt" ; 
    ESP_LOGI(TAG, "--- 开始读写测试 ---" ); 

    FILE *f = fopen(file_path, "w" ); 
    if (f == NULL ) { 
        ESP_LOGE(TAG, "❌ 打开文件写入失败！" ); 
        return ; 
    } 
    fprintf(f, "Hello Modular SDMMC Architecture!\n" ); 
    fclose(f); 
    ESP_LOGI(TAG, "✅ 文件写入成功！" ); 

    f = fopen(file_path, "r" ); 
    if (f == NULL ) { 
        ESP_LOGE(TAG, "❌ 打开文件读取失败！" ); 
        return ; 
    } 
    char line[128 ]; 
    if (fgets(line, sizeof(line), f) != NULL ) { 
        char *pos = strchr(line, '\n' ); 
        if (pos) { *pos = '\0' ; } 
        ESP_LOGI(TAG, "📖 成功读取内容: '%s'" , line); 
    } 
    fclose(f); 
} 

// 终极版文本净化器：杀尽英文与隐形控制符 
void clean_text_for_tts(char *str) { 
    char  *src = str, *dst = str; 
    while  (*src) { 
        if ((*src >= 'a' && *src <= 'z' ) || 
            (*src >= 'A' && *src <= 'Z' ) || 
            (*src > 0 && *src < 32 )) {  
            src++; // 遇到这些毒药，直接跳过 
        } else  { 
            *dst++ = *src++; // 合法的中文和全角标点，放行 
        } 
    } 
    *dst = '\0'; // 重新封口 
} 

// ========================================== 
// 💡 读取块大小设定为 128 字节 
// ========================================== 
#define READ_CHUNK_SIZE 128 
#define NOVEL_FILE_PATH MOUNT_POINT"/novel.txt" 

// ✨ 全局书签：记录在 SD 卡文件中的绝对字节位置 
static uint32_t current_file_offset = 0 ; 

// ========================================== 
// 📖 纯净版：从 SD 卡读取、TTS播报、并通过串口发给 UI 
// ========================================== 
void test_read_novel_next_chunk(void) { 
    FILE *f = fopen(NOVEL_FILE_PATH, "r" ); 
    if (f == NULL ) { 
        ESP_LOGE("SD_READ", "❌ 找不到文件: %s" , NOVEL_FILE_PATH); 
        return ; 
    } 

    // 1. 【翻书】：跳到上次读到的字节位置 
    fseek(f, current_file_offset, SEEK_SET); 

    // 2. 【看字】：捞取指定大小的字节 
    char read_buffer[READ_CHUNK_SIZE + 1 ]; 
    size_t bytes_read = fread(read_buffer, 1 , READ_CHUNK_SIZE, f); 

    // 检查是否读到了文件大结局 
    if (bytes_read == 0 ) { 
        ESP_LOGI("SD_READ", "🎉 恭喜，全书完！" ); 
        fclose(f); 
        return ; 
    } 

    // 3. 【防乱码截断】：处理 UTF-8 边界 
    int  valid_len = bytes_read; 
    
    if  (bytes_read == READ_CHUNK_SIZE) { 
        while (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0x80 ) { 
            valid_len--; 
        } 
        if (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0xC0 ) { 
            valid_len--; 
        } 
    } 

    // 4. 【封口并更新书签】 
    read_buffer[valid_len] = '\0' ; 
    current_file_offset += valid_len; 
    
    fclose(f); 
    clean_text_for_tts(read_buffer); // 清理乱码 
    
    ESP_LOGI("SD_READ", "--- 当前书签: %lu ---" , current_file_offset); 
    printf("%s\n\n" , read_buffer); 

    // ====================================================== 
    // ✨ 核心改变：通过串口发送给 UI，带有 NOV: 前缀 
    // ====================================================== 
    // 分配一个稍微大一点的数组，用来装前缀 + 文本 
    char uart_send_buf[READ_CHUNK_SIZE + 10 ]; 
    sprintf(uart_send_buf, "NOV:%s" , read_buffer); 
    
    // 调用我们在 my_uart.c 写的发送函数 
    my_uart_send(uart_send_buf); 

    // 5. 【语音播报】 
    tts_speak(read_buffer); 
} 
