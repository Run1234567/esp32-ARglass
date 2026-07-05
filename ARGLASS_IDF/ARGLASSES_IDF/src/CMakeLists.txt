# =====================================================================
# 主应用组件 CMakeLists.txt
# =====================================================================
# 此文件将 src/main.c 注册为 ESP-IDF 的 "main" 组件。
# main 组件是应用程序的入口点，包含 app_main() 函数。
#
# 依赖的自定义组件:
#   - camera_app:       摄像头模块
#   - audio_app:        麦克风音频采集
#   - wifi_app:         WiFi STA 连接
#   - speaker_app:      扬声器输出
#   - my_uart:          UART 命令接口
#   - record_app:       录音模块
#   - tts_app:          TTS 语音合成
#   - music_app:        音乐播放器
#   - voice_app:        语音唤醒
# =====================================================================

# 自动收集 src/ 目录下所有 .c 源文件
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# 注册主组件
idf_component_register(
    SRCS ${app_sources}                    # 源文件列表
    INCLUDE_DIRS "."                       # 头文件搜索路径 (当前目录)
    PRIV_REQUIRES camera_app audio_app wifi_app speaker_app my_uart record_app tts_app music_app voice_app ai_chat lwip mqtt
    # PRIV_REQUIRES: 私有依赖，只在编译此组件时使用
)
