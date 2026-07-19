idf_component_register(SRCS "tft_display.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver esp_driver_i2c esp_driver_gpio)
