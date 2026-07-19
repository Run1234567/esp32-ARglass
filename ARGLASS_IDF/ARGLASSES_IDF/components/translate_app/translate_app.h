#ifndef TRANSLATE_APP_H
#define TRANSLATE_APP_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化翻译应用（传入 main.c 创建的 PSRAM ringbuf）
 */
void translate_app_init(RingbufHandle_t rb);

/**
 * @brief 开始翻译会话
 * @param server_ip  服务器 IP（NULL 使用默认 124.220.224.189）
 * @param port       端口（0 使用默认 5001）
 */
void translate_start(const char *server_ip, int port);

/**
 * @brief 停止翻译会话
 */
void translate_stop(void);

/**
 * @brief 是否正在翻译
 */
bool translate_is_active(void);

/**
 * @brief 动态设置翻译语言（在 translate_start 之前调用生效）
 * @param src 源语言代码 (如 "en", "zh", "ja", "ko")
 * @param tgt 目标语言代码
 */
void translate_set_language(const char *src, const char *tgt);

/**
 * @brief 设置翻译模式
 * @param mode 模式: "s2s"=同声传译, "s2t"=语音转文本
 */
void translate_set_mode(const char *mode);

#ifdef __cplusplus
}
#endif

#endif // TRANSLATE_APP_H
