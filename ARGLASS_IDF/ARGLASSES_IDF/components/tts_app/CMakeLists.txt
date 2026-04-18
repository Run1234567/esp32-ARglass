idf_component_register(
    SRCS "tts_app.c" "voice_data.c"  # <--- 确保这两个文件都在这里
    INCLUDE_DIRS "."
    REQUIRES esp-sr speaker_app 
)