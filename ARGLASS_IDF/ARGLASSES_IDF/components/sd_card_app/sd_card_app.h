/**
 * @file sd_card_app.h
 * @brief SD 卡管理模块公共接口
 *
 * 本模块提供 SD 卡挂载、小说阅读、音乐/歌词扫描等功能。
 * SD 卡通过 SDMMC 1-bit 模式连接，挂载点为 "/sdcard"。
 */

#ifndef SD_CARD_APP_H
#define SD_CARD_APP_H

#include "esp_err.h"

/* =====================================================================
 * SD 卡挂载点
 * =====================================================================
 * 所有文件操作都基于此路径，例如:
 *   fopen("/sdcard/小说/三体/第一章.txt", "r")
 */
#define MOUNT_POINT "/sdcard"

/* =====================================================================
 * 小说阅读系统全局变量
 * ===================================================================== */

/**
 * @brief 全局书签：当前阅读位置的绝对字节偏移量
 * 每次读取 128 字节后自动递增
 */
extern uint32_t current_file_offset;

/**
 * @brief 当前正在阅读的小说文件绝对路径
 * 例如: "/sdcard/小说/三体/第一章.txt"
 * 由 my_uart 模块在收到 CMD:READ_CHAP: 命令时设置
 */
extern char current_novel_path[128];

/**
 * @brief TTS 模式开关 (1=TTS语音播报, 0=纯文本显示)
 * 由 my_uart 模块在收到 CMD:MODE:TTS/TEXT 命令时切换
 */
extern uint8_t global_tts_enabled;

/* =====================================================================
 * 函数声明
 * ===================================================================== */

/**
 * @brief 初始化 SD 卡 (SDMMC 1-bit 模式)
 * @return ESP_OK: 成功; 其他: 错误码
 */
esp_err_t init_sd_card(void);

/**
 * @brief SD 卡读写测试 (写入并读回 test.txt)
 */
void test_sd_card_read_write(void);

/**
 * @brief 读取下一段小说文本并播报/显示
 *
 * 从 current_novel_path 的 current_file_offset 位置读取 128 字节，
 * 处理 UTF-8 边界后，根据 global_tts_enabled 模式进行分发。
 * 由 novel_read_task 在收到 next_page_sem 信号时调用。
 */
void test_read_novel_next_chunk(void);

/**
 * @brief 扫描书籍列表并分页发送给 UI
 * @param offset 分页偏移量 (0=第一页)
 */
void scan_and_send_book_list(int offset);

/**
 * @brief 扫描章节列表并分页发送给 UI
 * @param book_name 书籍文件夹名
 * @param offset    分页偏移量
 */
void scan_and_send_chapter_list(const char* book_name, int offset);

/**
 * @brief 扫描音乐文件列表并发送给 UI
 * 扫描 /sdcard/音乐/ 下的 .mp3/.wav 文件
 */
void scan_and_send_music_list(void);

/**
 * @brief 解析 LRC 歌词文件并发送给 UI
 * @param song_name 歌曲文件名 (如 "歌曲1.wav")
 * 自动查找同名 .lrc 文件
 */
void send_lrc_to_ui(const char* song_name);

#endif // SD_CARD_APP_H
