#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h" 
#include "driver/gpio.h" 
#include "app_mqtt.h" // ? 引入 MQTT 模块的头文件，获取全局客户端句柄
// 引入自己的头文件
#include "sd_card_app.h"


static const char *TAG = "SD_APP";


#include "cJSON.h"          // ✨ 引入 ESP-IDF 自带的 cJSON 库
#include "mqtt_client.h"    // ✨ 引入 ESP-IDF 自带的 MQTT 客户端库

// ==========================================
// 📡 封装函数：将小说文本打包成 JSON 并通过 MQTT 发送
// ==========================================
void send_novel_chunk_via_mqtt(esp_mqtt_client_handle_t client, const char *text_chunk) {
    if (client == NULL || text_chunk == NULL) {
        ESP_LOGE("MQTT_SEND", "❌ 客户端未连接或文本为空！");
        return;
    }

    // 1. 创建 JSON 根对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;

    // 2. 添加数据节点 (cJSON 会自动处理文本里的特殊字符转义)
    cJSON_AddStringToObject(root, "cmd", "novel");
    cJSON_AddStringToObject(root, "data", text_chunk);

    // 3. 将 JSON 对象压缩成字符串 (PrintUnformatted 省空间，不带多余空格换行)
    char *json_string = cJSON_PrintUnformatted(root);

    if (json_string != NULL) {
        // 4. 发送给 AR 眼镜的专属 Topic (QoS=1 保证送达)
        int msg_id = esp_mqtt_client_publish(client, "jarvis/glasses/display", json_string, 0, 1, 0);
        
        ESP_LOGI("MQTT_SEND", "✅ 成功发送数据包 [ID:%d], 负载大小: %d 字节", msg_id, strlen(json_string));
        
        // 5. ⚠️ 极其重要：释放 Print 生成的字符串内存，否则会导致内存泄漏！
        free(json_string);
    } else {
        ESP_LOGE("MQTT_SEND", "❌ JSON 格式化失败，可能内存不足！");
    }

    // 6. 清理 JSON 对象树
    cJSON_Delete(root);
}


esp_err_t init_sd_card(void) {
    esp_err_t ret;
    sdmmc_card_t *card;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "正在初始化原生 SDMMC 总线...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; 

    // 官方 Sense 扩展板引脚映射
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;         
    slot_config.clk = 7;           
    slot_config.cmd = 9;           
    slot_config.d0  = 8;           
    slot_config.d1 = -1;
    slot_config.d2 = -1;
    slot_config.d3 = -1; 
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; 

    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败，错误码: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "🌟 模块化原生 SDMMC 挂载成功！");
    sdmmc_card_print_info(stdout, card);
    
    return ESP_OK;
}

void test_sd_card_read_write(void) {
    const char *file_path = MOUNT_POINT"/test.txt";
    ESP_LOGI(TAG, "--- 开始读写测试 ---");

    FILE *f = fopen(file_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 打开文件写入失败！");
        return;
    }
    fprintf(f, "Hello Modular SDMMC Architecture!\n");
    fclose(f);
    ESP_LOGI(TAG, "✅ 文件写入成功！");

    f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 打开文件读取失败！");
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), f) != NULL) {
        char *pos = strchr(line, '\n');
        if (pos) { *pos = '\0'; }
        ESP_LOGI(TAG, "📖 成功读取内容: '%s'", line);
    }
    fclose(f);
}


// ==========================================
// 💡 修改这里：将读取块大小设定为 1024 字节 (1KB)
// 这刚好能保证覆盖甚至略微超出“两个完整屏幕”的中文字数
// ==========================================
#define READ_CHUNK_SIZE 1024 
#define NOVEL_FILE_PATH MOUNT_POINT"/novel.txt"

// ✨ 全局书签：记录在 SD 卡文件中的绝对字节位置
static uint32_t current_file_offset = 0; 

// ==========================================
// 📖 纯净版：从 SD 卡读取下一段安全文本并打印
// ==========================================
void test_read_novel_next_chunk(void) {
    FILE *f = fopen(NOVEL_FILE_PATH, "r");
    if (f == NULL) {
        ESP_LOGE("SD_READ", "❌ 找不到文件: %s", NOVEL_FILE_PATH);
        return;
    }

    // 1. 【翻书】：跳到上次读到的字节位置
    fseek(f, current_file_offset, SEEK_SET);

    // 2. 【看字】：捞取指定大小的字节 (现在一次捞 1024 字节)
    char read_buffer[READ_CHUNK_SIZE + 1];
    size_t bytes_read = fread(read_buffer, 1, READ_CHUNK_SIZE, f);

    // 检查是否读到了文件大结局
    if (bytes_read == 0) {
        ESP_LOGI("SD_READ", "🎉 恭喜，全书完！");
        fclose(f);
        return;
    }

    // 3. 【防乱码截断】：处理 UTF-8 边界
    // 哪怕我们读了 1024 字节，如果第 1024 个字节刚好切在汉字中间，
    // 下面这段神仙逻辑依然会让它安全回退到第 1022 或 1021 个字节！
    int valid_len = bytes_read;
    
    // 如果还没到文件末尾，执行安全回退
    if (bytes_read == READ_CHUNK_SIZE) {
        // UTF-8 的延续字节特征是 10xxxxxx (即 0x80 到 0xBF)
        while (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0x80) {
            valid_len--; 
        }
        // 找到了汉字的首字节 (例如 1110xxxx)，也把它砍掉留给下一次
        if (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0xC0) {
            valid_len--; 
        }
    }

    // 4. 【封口并更新书签】
    read_buffer[valid_len] = '\0';
    current_file_offset += valid_len; // 书签加上这次有效读取的字节数
    
    fclose(f);

    // 5. 【展示结果】：打印到串口
    ESP_LOGI("SD_READ", "--- 当前书签: %lu ---", current_file_offset);
    printf("%s\n\n", read_buffer); 
    send_novel_chunk_via_mqtt(mqtt_client, read_buffer);
}