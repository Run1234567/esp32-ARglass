/**
 * @file voice_app.h
 * @brief "Jarvis" 唤醒词检测模块公共接口
 *
 * 本模块使用 ESP-SR WakeNet9 实现 "贾维斯" 唤醒词检测。
 * 内部创建两个 FreeRTOS 任务 (Core 0)：音频喂入和唤醒检测。
 */

#ifndef VOICE_APP_H
#define VOICE_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 "Jarvis" 唤醒词检测引擎
 *
 * 初始化 ESP-SR 模型、AFE 音频前端、WakeNet9 唤醒网络，
 * 然后启动两个 FreeRTOS 任务开始持续监听。
 *
 * 唤醒阈值设为 0.1 (较低)，对各种口音友好。
 * 检测到唤醒词后会打印日志 (TODO: 触发更多响应)。
 *
 * 必须在 app_main() 中调用，且在 SD 卡和音频初始化之后。
 */
void start_jarvis_brain(void);

#ifdef __cplusplus
}
#endif

#endif // VOICE_APP_H
