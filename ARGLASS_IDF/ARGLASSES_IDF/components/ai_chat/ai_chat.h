/**
 * @file ai_chat.h
 * @brief AI 语音对话模块公共接口
 *
 * 本模块实现 ESP32-S3 与云端 AI 服务器的 WebSocket 实时语音对话。
 * 流程：唤醒词检测 → OTA 发现 → WebSocket 连接 → Opus 音频流式收发
 */

#ifndef AI_CHAT_H
#define AI_CHAT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AI 对话状态枚举
 */
typedef enum {
    AI_CHAT_IDLE = 0,        // 空闲，未连接
    AI_CHAT_CONNECTING,      // 正在连接（OTA + WebSocket）
    AI_CHAT_CONNECTED,       // 已连接，等待服务器响应
    AI_CHAT_STREAMING,       // 正在流式传输音频
    AI_CHAT_ERROR,           // 连接出错
} ai_chat_state_t;

/**
 * @brief 初始化 AI 对话模块
 *
 * 创建任务、队列、信号量。在 app_main() 中调用一次。
 * 此时不会连接服务器，需要调用 ai_chat_start() 触发连接。
 */
void ai_chat_init(void);

/**
 * @brief 启动 AI 对话
 *
 * 唤醒词检测后调用。自动执行：
 *   1. OTA 发现（获取 WebSocket URL 和 token）
 *   2. 建立 WebSocket 连接
 *   3. Hello 握手
 *   4. 开始麦克风音频流式上传
 *
 * 此函数非阻塞，内部通过任务异步执行。
 */
void ai_chat_start(void);

/**
 * @brief 停止 AI 对话
 *
 * 断开 WebSocket 连接，停止音频流。
 */
void ai_chat_stop(void);

/**
 * @brief 查询 AI 对话是否活跃
 * @return true = 已连接或正在连接中
 */
bool ai_chat_is_active(void);

/**
 * @brief 获取当前 AI 对话状态
 * @return ai_chat_state_t 当前状态
 */
ai_chat_state_t ai_chat_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_H
