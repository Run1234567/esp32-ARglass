#ifndef TTS_APP_H
#define TTS_APP_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "freertos/semphr.h"

// 3. 跨文件声明：告诉所有包含此头文件的 C 文件，有这把锁存在！
extern SemaphoreHandle_t speaker_mutex;
/**
 * @brief 初始化贾维斯 TTS 语音中枢系统
 * 包含：初始化同步锁、音频缓冲区，并从 SD 卡加载声学模型到 PSRAM
 * 需要在 app_main 早期调用。
 */
void init_tts_engine(void);

/**
 * @brief 向语音系统追加新文本 (异步非阻塞)
 * 将文字放入文本缓冲区后立即返回，由后台任务自动逐字转换并播放。
 * @param new_text 需要播报的 UTF-8 字符串
 */
void jarvis_add_text(const char *new_text);

/**
 * @brief 文本转语音播报 (兼容旧版代码)
 * 内部已自动重定向到异步的 jarvis_add_text，防止原有代码报错。
 * @param text 需要播报的 UTF-8 字符串
 */
void tts_speak(const char *text);


#ifdef __cplusplus
}
#endif

#endif // TTS_APP_H