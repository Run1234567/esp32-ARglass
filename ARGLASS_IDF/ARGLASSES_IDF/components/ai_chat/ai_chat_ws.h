/**
 * @file ai_chat_ws.h
 * @brief WebSocket 客户端（OTA 发现 + WebSocket 连接）
 */

#ifndef AI_CHAT_WS_H
#define AI_CHAT_WS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WebSocket 消息回调
 * @param data 消息数据（文本或二进制）
 * @param len  数据长度
 * @param is_binary true=二进制帧, false=文本帧
 */
typedef void (*ws_msg_cb_t)(const char *data, int len, bool is_binary);

/**
 * @brief WebSocket 连接状态回调
 * @param connected true=已连接, false=已断开
 */
typedef void (*ws_state_cb_t)(bool connected);

/**
 * @brief 执行 OTA 发现，获取 WebSocket URL 和 token
 * @return 0=成功, -1=失败
 */
int ai_chat_ota_discover(void);

/**
 * @brief 建立 WebSocket 连接
 * @return 0=成功, -1=失败
 */
int ai_chat_ws_connect(void);

/**
 * @brief 断开 WebSocket 连接
 */
void ai_chat_ws_disconnect(void);

/**
 * @brief 发送文本消息
 * @param text JSON 文本
 * @return 0=成功, -1=失败
 */
int ai_chat_ws_send_text(const char *text);

/**
 * @brief 发送二进制数据
 * @param data 二进制数据
 * @param len  数据长度
 * @return 0=成功, -1=失败
 */
int ai_chat_ws_send_binary(const uint8_t *data, int len);

/**
 * @brief 查询 WebSocket 是否已连接
 */
bool ai_chat_ws_is_connected(void);

/**
 * @brief 设置消息回调
 */
void ai_chat_ws_set_callbacks(ws_msg_cb_t msg_cb, ws_state_cb_t state_cb);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_WS_H
