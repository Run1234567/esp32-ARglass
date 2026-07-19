# Opus source directory (local copy)
set(OPUS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/opus_src")

# Glob opus source files
file(GLOB OPUS_SOURCES "${OPUS_SRC_DIR}/*.c")

idf_component_register(
    SRCS
        "ai_chat.c"
        "ai_chat_ws.c"
        "ai_chat_protocol.c"
        "ai_chat_audio.c"
        ${OPUS_SOURCES}
    INCLUDE_DIRS
        "."
        "${OPUS_SRC_DIR}"
    REQUIRES
        esp_ringbuf
    PRIV_REQUIRES
        esp_http_client
        esp_event
        cjson
        freertos
        log
        speaker_app
        tts_app
        my_uart
)

# Opus compile definitions (already in opus_src/config.h, only add missing ones)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_LRINT
    HAVE_LRINTF
)

# Disable strict warnings + 强制 -O2 优化 (Opus DSP 运算密集，-Os 会严重卡顿)
set_source_files_properties(${OPUS_SOURCES} PROPERTIES
    COMPILE_FLAGS "-O2 -Wno-error -Wno-stringop-overread -Wno-unused-parameter -Wno-sign-compare -Wno-error=maybe-uninitialized -Wno-maybe-uninitialized"
)
