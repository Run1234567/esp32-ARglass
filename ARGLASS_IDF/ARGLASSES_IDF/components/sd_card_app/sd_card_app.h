#ifndef SD_CARD_APP_H
#define SD_CARD_APP_H

#include "esp_err.h"

// 挂载点路径定义
#define MOUNT_POINT "/sdcard"

// 初始化并挂载 SD 卡
esp_err_t init_sd_card(void);

// 测试：写入并读取一个文本文件
void test_sd_card_read_write(void);

#endif