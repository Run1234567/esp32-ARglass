#ifndef AUDIO_APP_H
#define AUDIO_APP_H

#include <stdint.h>
#include <stddef.h>

// 暴露给外部的函数
void initAudio(void);
size_t readAudio(int16_t* buffer, size_t samples);

#endif