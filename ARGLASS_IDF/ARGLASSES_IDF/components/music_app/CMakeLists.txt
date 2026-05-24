idf_component_register(SRCS "music_app.c"
                    INCLUDE_DIRS "."
                    REQUIRES speaker_app tts_app my_uart)