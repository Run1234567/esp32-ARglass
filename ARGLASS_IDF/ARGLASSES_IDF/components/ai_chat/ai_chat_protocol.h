/**
 * @file ai_chat_protocol.h
 * @brief 消息协议（hello/listen/abort/tts/stt/llm/mcp）
 */

#ifndef AI_CHAT_PROTOCOL_H
#define AI_CHAT_PROTOCOL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 发送 hello 握手消息
 * @return 0=成功, -1=失败
 */
int proto_send_hello(void);

/**
 * @brief 发送 listen start 消息（开始监听语音）
 * @return 0=成功, -1=失败
 */
int proto_send_listen_start(void);

/**
 * @brief 发送 listen detect 消息（唤醒词检测到）
 * @param text 唤醒词文本（如 "嘿，你好呀"）
 * @return 0=成功, -1=失败
 */
int proto_send_listen_detect(const char *text);

/**
 * @brief 发送 abort 消息（打断服务器语音）
 * @return 0=成功, -1=失败
 */
int proto_send_abort(void);

/**
 * @brief 处理服务器下发的文本消息
 * @param data JSON 字符串
 * @param len  字符串长度
 */
void proto_handle_server_text(const char *data, int len);

/**
 * @brief 处理服务器下发的二进制消息（Opus 音频帧）
 * @param data Opus 数据
 * @param len  数据长度
 */
void proto_handle_server_binary(const char *data, int len);

/**
 * @brief 重置协议状态（断开连接时调用）
 */
void proto_reset(void);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_PROTOCOL_H
