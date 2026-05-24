#ifndef RECORD_APP_H
#define RECORD_APP_H

#include "esp_err.h"

// ? һ����ʼ¼�� (�Զ����� REC_xxx.wav)
esp_err_t start_record(void);

// ? ֹͣ¼��
void stop_record(void);

// ? ���չ��ܽӿ� (һ��ץ�Ĳ��Զ����� IMG_xxx.jpg)
esp_err_t take_photo_and_save(void);

// 📂 扫描 ly 文件夹并通过串口发送文件列表 
void scan_and_send_record_list(void);

#endif // RECORD_APP_H