# =====================================================================
# 语音唤醒模块 CMakeLists.txt
# =====================================================================
# 源文件: voice_app.c
# 依赖:
#   - esp-sr:      ESP 语音识别库 (WakeNet9 + AFE)
#   - freertos:    FreeRTOS 任务管理
#   - log:         日志输出
#   - esp_ringbuf: 环形缓冲区
#   - tts_app:     TTS 模块 (speaker_mutex 引用)
# =====================================================================

idf_component_register(SRCS "voice_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-sr freertos log esp_ringbuf tts_app ai_chat)
