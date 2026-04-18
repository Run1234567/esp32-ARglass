idf_component_register(SRCS "sd_card_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc vfs driver log json app_mqtt tts_app)