#ifndef VOICE_APP_H
#define VOICE_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动贾维斯的离线语音大脑
 * 初始化 AFE 降噪前端、唤醒词引擎，并启动配套的 FreeRTOS 任务
 */
void start_jarvis_brain(void);

#ifdef __cplusplus
}
#endif

#endif // VOICE_APP_H