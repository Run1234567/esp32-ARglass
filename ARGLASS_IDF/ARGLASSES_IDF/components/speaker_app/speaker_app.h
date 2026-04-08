#ifndef SPEAKER_APP_H
#define SPEAKER_APP_H

#include <stdint.h>
#include <stddef.h>

// 初始化喇叭
void initSpeaker(void);

// 播放音频数据 (传入数据指针和长度)
void playSpeaker(const uint8_t *data, size_t length);

#endif