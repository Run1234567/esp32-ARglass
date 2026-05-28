#ifndef SD_CARD_APP_H
#define SD_CARD_APP_H

#include "esp_err.h"

#define MOUNT_POINT "/sdcard"

extern uint32_t current_file_offset;
extern char current_novel_path[128];
extern uint8_t global_tts_enabled;

esp_err_t init_sd_card(void);
void test_sd_card_read_write(void);
void test_read_novel_next_chunk(void);
void scan_and_send_book_list(int offset);
void scan_and_send_chapter_list(const char* book_name, int offset);
void scan_and_send_music_list(void);
void send_lrc_to_ui(const char* song_name);

#endif // SD_CARD_APP_H