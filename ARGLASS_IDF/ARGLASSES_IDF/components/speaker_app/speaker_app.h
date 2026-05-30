#ifndef SPEAKER_APP_H
#define SPEAKER_APP_H

#include <stdint.h>
#include <stddef.h>

// ��ʼ������
void initSpeaker(void);

// ������Ƶ���� (��������ָ��ͳ���)
void playSpeaker(const uint8_t *data, size_t length);
// �� speaker_app.h �м��ϣ�
void set_speaker_volume(uint8_t vol); // ���� 0 �� 100
uint8_t get_speaker_volume(void);
#endif