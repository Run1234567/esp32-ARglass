idf_component_register(SRCS "sd_card_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc vfs driver log tts_app my_uart)