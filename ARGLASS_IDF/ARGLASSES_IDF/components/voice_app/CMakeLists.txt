idf_component_register(SRCS "voice_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp-sr freertos log esp_ringbuf tts_app)