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
    PRIV_REQUIRES
        esp_http_client
        esp_event
        json
        freertos
        log
        speaker_app
        tts_app
)

# Opus compile definitions (already in opus_src/config.h, only add missing ones)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_LRINT
    HAVE_LRINTF
)

# Disable strict warnings for opus source files
set_source_files_properties(${OPUS_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-error -Wno-stringop-overread -Wno-unused-parameter -Wno-sign-compare"
)
