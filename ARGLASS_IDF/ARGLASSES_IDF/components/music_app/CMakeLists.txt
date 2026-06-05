# =====================================================================
# WAV 音乐播放器模块 CMakeLists.txt
# =====================================================================
# 源文件: music_app.c
# 依赖:
#   - speaker_app: 扬声器播放
#   - tts_app:     TTS 模块 (speaker_mutex 互斥锁)
#   - my_uart:     串口通信 (上报播放进度)
# =====================================================================

idf_component_register(SRCS "music_app.c"
                    INCLUDE_DIRS "."
                    REQUIRES speaker_app tts_app my_uart)
