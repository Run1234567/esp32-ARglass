idf_component_register(SRCS "record_app.c"
                       INCLUDE_DIRS "."
                       # 告诉编译器：本模块需要使用 SD卡、音频 以及 官方摄像头底层驱动
                       REQUIRES sd_card_app audio_app esp32-camera)