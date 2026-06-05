# =====================================================================
# TTS 语音合成模块 CMakeLists.txt
# =====================================================================
# 源文件: tts_app.c, voice_data.c (TTS 语音模型的 C 数组备选)
# 依赖:
#   - esp-sr:      ESP 语音识别库 (包含 ESP-TTS 引擎)
#   - speaker_app: 扬声器播放
#   - my_uart:     串口通信 (小说模式下发送文本给 UI)
# =====================================================================

idf_component_register(
    SRCS "tts_app.c" "voice_data.c"  # 两个源文件: TTS逻辑 + 语音数据
    INCLUDE_DIRS "."
    REQUIRES esp-sr speaker_app my_uart
)
