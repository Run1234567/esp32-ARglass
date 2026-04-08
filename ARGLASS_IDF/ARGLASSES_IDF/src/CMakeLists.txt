FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES camera_app audio_app wifi_app speaker_app esp_websocket_client # ? 在这里加上 audio_app
)