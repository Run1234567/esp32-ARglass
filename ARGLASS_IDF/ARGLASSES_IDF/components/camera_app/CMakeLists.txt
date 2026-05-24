idf_component_register(SRCS "camera_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera log sd_card_app my_uart driver)