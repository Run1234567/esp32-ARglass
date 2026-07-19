idf_component_register(
    SRCS "translate_app.c"
    INCLUDE_DIRS "."
    REQUIRES esp_websocket_client cjson esp_ringbuf freertos log
    PRIV_REQUIRES esp_event
)
