/**
 * @file tts_app.h
 * @brief 中文 TTS 语音合成模块公共接口
 *
 * 本模块提供中文文本转语音功能，使用乐鑫 ESP-TTS 引擎和 "小乐" 语音。
 * 支持异步队列式播放、可变语速、强制打断等功能。
 */

#ifndef TTS_APP_H
#define TTS_APP_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/semphr.h"

/* =====================================================================
 * 全局变量
 * ===================================================================== */

/**
 * @brief 扬声器互斥锁 (在 main.c 中创建)
 *
 * TTS、音乐播放、WebSocket 音频下行 都需要使用扬声器，
 * 通过此互斥锁保证同一时刻只有一个任务在写 I2S 数据。
 */
extern SemaphoreHandle_t speaker_mutex;

/**
 * @brief TTS 语速等级 (0-9, 默认 4)
 * 由 tts_set_speed() 修改，tts_main_task 读取
 */
extern volatile int global_tts_speed;

/**
 * @brief 小说阅读模式标志
 * true = 连续阅读模式 (TTS 播完自动请求下一页)
 * false = 单次播放模式
 */
extern volatile bool is_reading_active;

/* =====================================================================
 * 函数声明
 * ===================================================================== */

/**
 * @brief 初始化 TTS 引擎
 *
 * 从 SD 卡加载语音模型到 PSRAM，创建 TTS 引擎句柄，
 * 启动 tts_main_task 合成任务 (Core 0, 32KB 栈)。
 * 必须在 app_main() 中调用一次，且在 SD 卡初始化之后。
 */
void init_tts_engine(void);

/**
 * @brief 将文本送入 TTS 队列 (异步播放)
 *
 * 自动按标点切句 (最大 90 字节/句)，放入消息队列。
 * 后台 tts_main_task 会自动取出并合成播放。
 * 此函数是非阻塞的 (队列满时会阻塞等待)。
 *
 * @param text 要合成的 UTF-8 中文文本
 */
void tts_speak(const char *text);

/**
 * @brief jarvis_add_text 的别名 (兼容旧接口)
 * @param new_text 要合成的文本
 */
void jarvis_add_text(const char *new_text);

/**
 * @brief 设置 TTS 语速
 * @param speed 语速等级 (0=最慢, 4=默认, 9=最快)
 */
void tts_set_speed(int speed);

/**
 * @brief 强制停止 TTS 播放并清空队列
 *
 * 立即中断当前播放，清空等待队列中的所有句子。
 * 用于用户退出阅读界面或切换模式时。
 */
void stop_tts_reading(void);

#ifdef __cplusplus
}
#endif

#endif // TTS_APP_H
