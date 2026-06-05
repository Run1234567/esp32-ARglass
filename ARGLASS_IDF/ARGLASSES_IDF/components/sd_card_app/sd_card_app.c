/**
 * @file sd_card_app.c
 * @brief SD 卡管理、小说阅读、音乐/LRC 歌词扫描模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 本模块是 SD 卡文件系统的核心管理层，提供以下功能：
 *
 *   1. SD 卡初始化与挂载 (SDMMC 1-bit 模式)
 *      - 引脚: CLK=GPIO7, CMD=GPIO9, D0=GPIO8
 *      - 挂载点: /sdcard
 *
 *   2. 小说/电子书阅读系统
 *      - 从 /sdcard/小说/ 目录读取 UTF-8 中文 TXT 文件
 *      - 每次读取 128 字节，自动处理 UTF-8 边界 (防乱码)
 *      - 文本净化：去除英文和控制字符 (TTS 只处理中文)
 *      - 支持 TTS 语音播报和纯文本两种模式
 *      - 全局书签 (current_file_offset) 记录阅读进度
 *
 *   3. 书籍/章节列表扫描 (分页)
 *      - 扫描 /sdcard/小说/ 下的子文件夹 (每本书)
 *      - 扫描每本书下的 .txt 文件 (每章)
 *      - 每页最多 6 条，通过 UART 发送给 UI
 *
 *   4. 音乐文件列表扫描
 *      - 扫描 /sdcard/音乐/ 下的 .mp3/.wav 文件
 *      - 通过 UART 发送给 UI
 *
 *   5. LRC 歌词文件解析与发送
 *      - 读取与歌曲同名的 .lrc 文件
 *      - 解析 [mm:ss] 格式的时间戳和歌词文本
 *      - 通过 UART 发送给 UI 实现歌词同步显示
 *
 * SD 卡目录结构：
 *   /sdcard/
 *   ├── 小说/
 *   │   ├── 书名A/
 *   │   │   ├── 第一章.txt
 *   │   │   ├── 第二章.txt
 *   │   │   └── ...
 *   │   └── 书名B/
 *   │       └── ...
 *   ├── 音乐/
 *   │   ├── 歌曲1.wav
 *   │   ├── 歌曲1.lrc    (配套歌词)
 *   │   └── ...
 *   ├── 录音/
 *   │   └── REC_001.wav
 *   ├── 拍照/
 *   │   └── IMG_001.jpg
 *   └── PZ/
 *       └── IMG_001.jpg
 *
 * 依赖组件：
 *   - fatfs:    FAT 文件系统
 *   - sdmmc:    SDMMC 主机驱动
 *   - vfs:      虚拟文件系统
 *   - driver:   GPIO 驱动
 *   - tts_app:  TTS 引擎 (用于小说语音播报)
 *   - my_uart:  串口通信 (发送数据给 UI)
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>       // 目录遍历 (opendir/readdir/closedir)

/* ==================== ESP-IDF 文件系统头文件 ==================== */
#include "esp_log.h"
#include "esp_vfs_fat.h"          // FAT 文件系统 VFS 接口
#include "sdmmc_cmd.h"            // SD/MMC 命令 (卡信息打印等)
#include "driver/sdmmc_host.h"    // SDMMC 主机驱动
#include "driver/gpio.h"          // GPIO 驱动

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== 项目组件头文件 ==================== */
#include "sd_card_app.h"   // 自身头文件 (MOUNT_POINT 等定义)
#include "tts_app.h"       // TTS 引擎 (tts_speak 函数)
#include "my_uart.h"       // 串口通信 (my_uart_send 函数)

static const char *TAG = "SD_APP";  // 日志标签

/* =====================================================================
 * SD 卡初始化函数
 * =====================================================================
 * @brief 通过 SDMMC 1-bit 模式挂载 SD 卡到 VFS
 *
 * SDMMC 1-bit 模式引脚：
 *   - CLK (时钟):  GPIO 7
 *   - CMD (命令):  GPIO 9
 *   - D0  (数据):  GPIO 8
 *
 * 使用 ESP-IDF 的原生 SDMMC 驱动 (不是 SPI 模式)，
 * 速度更快，且不占用 SPI 总线。
 *
 * 挂载成功后，可以通过标准 C 文件操作 (fopen/fread/fwrite) 访问 SD 卡。
 *
 * @return ESP_OK: 挂载成功; 其他: 错误码
 */
esp_err_t init_sd_card(void) {
    esp_err_t ret;
    sdmmc_card_t *card;

    /* ---- 挂载配置 ---- */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,     // 挂载失败时不自动格式化 (保护数据)
        .max_files = 5,                       // 最大同时打开文件数
        .allocation_unit_size = 16 * 1024    // FAT 分配单元: 16KB
    };

    ESP_LOGI(TAG, "正在初始化原生 SDMMC 总线...");

    /* ---- SDMMC 主机配置 ---- */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();  // 使用默认 SDMMC 主机配置
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   // 使用默认频率 (20MHz)

    /* ---- SDMMC 插槽/引脚配置 ---- */
    // 官方 Sense 扩展板引脚映射
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;          // 1-bit 模式 (只用 D0)
    slot_config.clk = 7;            // 时钟: GPIO 7
    slot_config.cmd = 9;            // 命令: GPIO 9
    slot_config.d0 = 8;             // 数据0: GPIO 8
    slot_config.d1 = -1;            // 未使用
    slot_config.d2 = -1;            // 未使用
    slot_config.d3 = -1;            // 未使用
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // 启用内部上拉电阻

    /* ---- 挂载 SD 卡到 VFS ---- */
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败，错误码: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "🌟 模块化原生 SDMMC 挂载成功！");
    sdmmc_card_print_info(stdout, card);  // 打印卡信息 (容量、类型等)

    return ESP_OK;
}

/* =====================================================================
 * SD 卡读写测试
 * =====================================================================
 * @brief 简单的读写测试，验证 SD 卡工作正常
 *
 * 流程：写入测试文件 -> 读回内容 -> 打印验证
 * 测试文件: /sdcard/test.txt
 */
void test_sd_card_read_write(void) {
    const char *file_path = MOUNT_POINT"/test.txt";
    ESP_LOGI(TAG, "--- 开始读写测试 ---");

    /* ---- 写入测试 ---- */
    FILE *f = fopen(file_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 打开文件写入失败！");
        return;
    }
    fprintf(f, "Hello Modular SDMMC Architecture!\n");
    fclose(f);
    ESP_LOGI(TAG, "✅ 文件写入成功！");

    /* ---- 读取测试 ---- */
    f = fopen(file_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 打开文件读取失败！");
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), f) != NULL) {
        // 去除行尾换行符
        char *pos = strchr(line, '\n');
        if (pos) { *pos = '\0'; }
        ESP_LOGI(TAG, "📖 成功读取内容: '%s'", line);
    }
    fclose(f);
}

/* =====================================================================
 * 文本净化器 - 为 TTS 清理文本
 * =====================================================================
 * @brief 去除字符串中的英文字母和控制字符，只保留中文和标点
 *
 * ESP-TTS 引擎只能处理中文字符，英文字母会导致合成异常。
 * 此函数原地修改字符串，去除以下字符：
 *   - ASCII 英文字母 (a-z, A-Z)
 *   - 控制字符 (ASCII 0-31，如换行、制表符等)
 *
 * @param str 要清理的 UTF-8 字符串 (原地修改)
 */
void clean_text_for_tts(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        // 检查是否是需要跳过的字符
        if ((*src >= 'a' && *src <= 'z') ||      // 小写英文
            (*src >= 'A' && *src <= 'Z') ||       // 大写英文
            (*src > 0 && *src < 32)) {            // 控制字符 (ASCII 0-31)
            src++;  // 跳过这些字符
        } else {
            *dst++ = *src++;  // 合法的中文和全角标点，保留
        }
    }
    *dst = '\0';  // 重新封口
}

/* =====================================================================
 * 小说阅读系统 - 核心配置
 * ===================================================================== */
#define READ_CHUNK_SIZE 128  // 每次从 SD 卡读取的字节数

/**
 * @brief 全局书签：记录在 SD 卡文件中的绝对字节位置
 * 每次读取后自动递增，用于下次读取时 fseek 定位
 */
uint32_t current_file_offset = 0;

/**
 * @brief 当前正在阅读的小说绝对路径
 * 例如: "/sdcard/小说/三体/第一章.txt"
 */
char current_novel_path[128] = "";

/**
 * @brief 全局开关：TTS 语音播报模式
 *   1 = TTS 语音播报模式 (文本 -> TTS 引擎 -> 扬声器)
 *   0 = 纯文本模式 (文本 -> UART -> UI 屏幕显示)
 */
uint8_t global_tts_enabled = 1;

/* =====================================================================
 * 小说文本读取与播报
 * =====================================================================
 * @brief 从 SD 卡读取下一段小说文本，根据模式进行 TTS 播报或发送给 UI
 *
 * 工作流程：
 *   1. 打开当前小说文件，fseek 到上次的书签位置
 *   2. 读取 128 字节
 *   3. 处理 UTF-8 边界：如果截断了多字节字符，向前回退
 *   4. 更新书签位置
 *   5. 清理文本 (去除英文/控制字符)
 *   6. 根据模式分发：
 *      - TTS 模式: 交给 tts_speak() 合成语音
 *      - 纯文本模式: 通过 UART 发送给 UI 显示
 *
 * 此函数由 novel_read_task (main.c) 在收到 next_page_sem 信号时调用
 */
void test_read_novel_next_chunk(void) {
    /* ---- 检查是否已指定阅读路径 ---- */
    if (strlen(current_novel_path) == 0) {
        ESP_LOGE("SD_READ", "❌ 未指定阅读路径");
        return;
    }

    /* ---- 步骤1: 打开文件并定位到书签 ---- */
    FILE *f = fopen(current_novel_path, "r");
    if (f == NULL) {
        ESP_LOGE("SD_READ", "❌ 找不到章节文件: %s", current_novel_path);
        return;
    }

    // 翻书：跳到上次读到的字节位置
    fseek(f, current_file_offset, SEEK_SET);

    /* ---- 步骤2: 读取指定大小的字节 ---- */
    char read_buffer[READ_CHUNK_SIZE + 1];
    size_t bytes_read = fread(read_buffer, 1, READ_CHUNK_SIZE, f);

    // 检查是否读到了文件末尾 (全书完)
    if (bytes_read == 0) {
        ESP_LOGI("SD_READ", "🎉 恭喜，全书完！");
        fclose(f);
        return;
    }

    /* ---- 步骤3: 防乱码截断 - 处理 UTF-8 边界 ---- */
    // UTF-8 多字节字符的后续字节都以 10xxxxxx (0x80-0xBF) 开头
    // 如果读取在多字节字符中间截断，需要向前回退到字符边界
    int valid_len = bytes_read;

    if (bytes_read == READ_CHUNK_SIZE) {
        // 向前回退，跳过不完整的 UTF-8 后续字节 (10xxxxxx)
        while (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0x80) {
            valid_len--;
        }
        // 回退一个起始字节 (110xxxxx, 1110xxxx, 11110xxx)
        if (valid_len > 0 && (read_buffer[valid_len - 1] & 0xC0) == 0xC0) {
            valid_len--;
        }
    }

    /* ---- 步骤4: 封口并更新书签 ---- */
    read_buffer[valid_len] = '\0';
    current_file_offset += valid_len;  // 更新全局书签

    fclose(f);

    /* ---- 步骤5: 清理文本 ---- */
    clean_text_for_tts(read_buffer);  // 去除英文和控制字符

    ESP_LOGI("SD_READ", "--- 当前书签: %lu ---", current_file_offset);

    /* ---- 步骤6: 根据模式分发文本 ---- */
    if (global_tts_enabled == 1) {
        // TTS 语音播报模式：交给 TTS 引擎合成并播放
        tts_speak(read_buffer);
    } else {
        // 纯文本模式：通过 UART 发送给 UI 屏幕显示
        char uart_send_buf[READ_CHUNK_SIZE + 10];
        sprintf(uart_send_buf, "NOV:%s", read_buffer);  // 格式: "NOV:文本内容"
        my_uart_send(uart_send_buf);
    }
}

/* =====================================================================
 * 音乐文件列表扫描
 * =====================================================================
 * @brief 扫描 /sdcard/音乐/ 目录，将所有音乐文件名发送给 UI
 *
 * 支持的格式: .mp3, .MP3, .wav, .WAV
 * 通信协议:
 *   1. 发送 "MU_CLEAR:1" 清空 UI 列表
 *   2. 逐个发送 "MU:文件名"
 *   3. 发送 "MU_END:1" 表示扫描完成
 */
void scan_and_send_music_list(void) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/音乐", MOUNT_POINT);

    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "无法打开 %s 文件夹！", dir_path);
        return;
    }

    struct dirent *ent;

    // 通知 UI 清空现有列表
    my_uart_send("MU_CLEAR:1");

    // 遍历目录中的所有文件
    while ((ent = readdir(dir)) != NULL) {
        // 检查文件扩展名是否为音乐格式
        if (strstr(ent->d_name, ".mp3") || strstr(ent->d_name, ".MP3") ||
            strstr(ent->d_name, ".wav") || strstr(ent->d_name, ".WAV")) {
            char cmd[300];
            snprintf(cmd, sizeof(cmd), "MU:%s", ent->d_name);
            my_uart_send(cmd);
            vTaskDelay(pdMS_TO_TICKS(20));  // 短暂延时，避免 UART 缓冲区溢出
        }
    }
    closedir(dir);

    // 通知 UI 列表发送完毕
    my_uart_send("MU_END:1");
    ESP_LOGI(TAG, "🎵 音乐列表扫描完成并已发送");
}

/* =====================================================================
 * LRC 歌词文件解析与发送
 * =====================================================================
 * @brief 读取与歌曲同名的 .lrc 歌词文件，解析后发送给 UI
 *
 * LRC 格式: [mm:ss]歌词文本
 * 例如:
 *   [00:12]这是第一句歌词
 *   [00:18]这是第二句歌词
 *
 * 解析流程：
 *   1. 将歌曲文件名的扩展名替换为 .lrc
 *   2. 发送 "LRC_CLR" 清空 UI 歌词
 *   3. 逐行解析 [mm:ss] 格式的时间戳和文本
 *   4. 转换为秒数后发送 "LRC:秒数:文本" 给 UI
 *
 * @param song_name 歌曲文件名 (如 "歌曲1.wav")
 */
void send_lrc_to_ui(const char* song_name) {
    /* ---- 构建 LRC 文件路径 ---- */
    char lrc_path[128];
    snprintf(lrc_path, sizeof(lrc_path), "%s/音乐/%s", MOUNT_POINT, song_name);

    // 将扩展名替换为 .lrc (如 "歌曲1.wav" -> "歌曲1.lrc")
    char *ext = strrchr(lrc_path, '.');
    if (ext != NULL) {
        strcpy(ext, ".lrc");
    }

    /* ---- 清空 UI 歌词显示 ---- */
    my_uart_send("LRC_CLR\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- 打开并解析 LRC 文件 ---- */
    FILE *f = fopen(lrc_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "❌ 找不到配套歌词: %s", lrc_path);
        return;
    }

    ESP_LOGI(TAG, "📖 找到歌词文件，开始推送到 UI...");
    char line_buf[256];

    // 逐行读取 LRC 文件
    while (fgets(line_buf, sizeof(line_buf), f) != NULL) {
        int m = 0, s = 0;

        // 解析时间戳 [mm:ss]
        if (sscanf(line_buf, "[%d:%d", &m, &s) == 2) {
            // 找到 ] 后面的歌词文本
            char *text_start = strchr(line_buf, ']');
            if (text_start != NULL) {
                text_start++;                          // 跳过 ]
                while(*text_start == ' ') text_start++; // 跳过空格
                text_start[strcspn(text_start, "\r\n")] = '\0';  // 去除换行

                // 空歌词用空格代替 (UI 需要收到数据才能同步时间)
                if (strlen(text_start) == 0) text_start = " ";

                // 将 mm:ss 转换为总秒数
                int time_sec = m * 60 + s;

                // 发送给 UI: "LRC:秒数:歌词文本"
                char cmd[300];
                snprintf(cmd, sizeof(cmd), "LRC:%d:%s\r\n", time_sec, text_start);
                my_uart_send(cmd);

                vTaskDelay(pdMS_TO_TICKS(15));  // 延时防止 UART 溢出
            }
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "✅ 歌词推送完成！");
}

/* =====================================================================
 * 分页扫描配置
 * ===================================================================== */
#define PAGE_LIMIT 6  // 每页最多显示的条目数 (根据 UI 屏幕大小调整)

/* =====================================================================
 * 书籍列表扫描 (分页)
 * =====================================================================
 * @brief 扫描 /sdcard/小说/ 目录下的子文件夹 (每本书)，分页发送给 UI
 *
 * 通信协议：
 *   1. "BK_CLR"         - 清空 UI 书库列表
 *   2. "BK_PAGE:PREV"   - 显示"上一页"按钮 (非第一页时)
 *   3. "BK:书名"        - 每本书的文件夹名
 *   4. "BK_PAGE:NEXT"   - 显示"下一页"按钮 (还有更多时)
 *   5. "BK_END"         - 列表发送完毕
 *
 * @param offset 分页偏移量 (0=第一页, 6=第二页, ...)
 */
void scan_and_send_book_list(int offset) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/小说", MOUNT_POINT);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE("SD_APP", "❌ 无法打开 小说 文件夹");
        return;
    }

    // 清空 UI 列表
    my_uart_send("BK_CLR\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));

    // 如果不是第一页，发送"上一页"按钮
    if (offset > 0) {
        my_uart_send("BK_PAGE:PREV\r\n");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    struct dirent *ent;
    int current_idx = 0;   // 当前遍历到的目录索引
    int sent_count = 0;    // 本页已发送的条目数
    bool has_more = false; // 是否还有更多数据 (显示"下一页"按钮)

    while ((ent = readdir(dir)) != NULL) {
        // 只处理目录 (每本书是一个文件夹)
        if (ent->d_type == DT_DIR) {
            // 跳过 . 和 .. 特殊目录
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            // 跳过 offset 之前的条目 (已经显示过的)
            if (current_idx >= offset) {
                if (sent_count < PAGE_LIMIT) {
                    // 发送书名: "BK:书名"
                    char cmd[300];
                    snprintf(cmd, sizeof(cmd), "BK:%s\r\n", ent->d_name);
                    my_uart_send(cmd);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    sent_count++;
                } else {
                    // 本页已满，标记还有更多
                    has_more = true;
                    break;
                }
            }
            current_idx++;
        }
    }
    closedir(dir);

    // 如果还有更多数据，发送"下一页"按钮
    if (has_more) {
        my_uart_send("BK_PAGE:NEXT\r\n");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // 发送完毕标记
    my_uart_send("BK_END\r\n");
    ESP_LOGI("SD_APP", "📚 书库列表 (偏移量:%d) 发送完毕", offset);
}

/* =====================================================================
 * 章节列表扫描 (分页)
 * =====================================================================
 * @brief 扫描指定书籍文件夹下的 .txt 文件 (每章)，分页发送给 UI
 *
 * @param book_name 书籍文件夹名 (如 "三体")
 * @param offset    分页偏移量
 *
 * 通信协议：
 *   1. "CH_CLR"         - 清空 UI 章节列表
 *   2. "CH_PAGE:PREV"   - 上一页按钮
 *   3. "CH:章节名.txt"  - 每个章节文件
 *   4. "CH_PAGE:NEXT"   - 下一页按钮
 *   5. "CH_END"         - 列表发送完毕
 */
void scan_and_send_chapter_list(const char* book_name, int offset) {
    char dir_path[128];
    snprintf(dir_path, sizeof(dir_path), "%s/小说/%s", MOUNT_POINT, book_name);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE("SD_APP", "❌ 无法打开书籍文件夹: %s", dir_path);
        return;
    }

    // 清空 UI 列表
    my_uart_send("CH_CLR\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));

    // 如果不是第一页，发送"上一页"按钮
    if (offset > 0) {
        my_uart_send("CH_PAGE:PREV\r\n");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    struct dirent *ent;
    int current_idx = 0;
    int sent_count = 0;
    bool has_more = false;

    while ((ent = readdir(dir)) != NULL) {
        // 只处理 .txt 文件
        if (strstr(ent->d_name, ".txt") || strstr(ent->d_name, ".TXT")) {
            if (current_idx >= offset) {
                if (sent_count < PAGE_LIMIT) {
                    // 发送章节名: "CH:章节名.txt"
                    char cmd[300];
                    snprintf(cmd, sizeof(cmd), "CH:%s\r\n", ent->d_name);
                    my_uart_send(cmd);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    sent_count++;
                } else {
                    has_more = true;
                    break;
                }
            }
            current_idx++;
        }
    }
    closedir(dir);

    // 如果还有更多，发送"下一页"按钮
    if (has_more) {
        my_uart_send("CH_PAGE:NEXT\r\n");
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // 发送完毕标记
    my_uart_send("CH_END\r\n");
    ESP_LOGI("SD_APP", "📑 章节列表 (偏移量:%d) 发送完毕", offset);
}
