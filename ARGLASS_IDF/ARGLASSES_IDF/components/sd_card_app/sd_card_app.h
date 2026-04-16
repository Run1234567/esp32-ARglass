#ifndef SD_CARD_APP_H
#define SD_CARD_APP_H

#include "esp_err.h"

// 将挂载点宏定义公开，方便其他模块知道存在哪
#define MOUNT_POINT "/sdcard"

// 对外暴露的两个函数接口
esp_err_t init_sd_card(void);
void test_sd_card_read_write(void);
void test_read_novel_next_chunk(void);
#endif // SD_CARD_APP_H