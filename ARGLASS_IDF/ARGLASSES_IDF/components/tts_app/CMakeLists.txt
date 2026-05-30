idf_component_register(
    SRCS "tts_app.c" "voice_data.c"  # <--- ȷ���������ļ���������
    INCLUDE_DIRS "."
    REQUIRES esp-sr speaker_app my_uart
)