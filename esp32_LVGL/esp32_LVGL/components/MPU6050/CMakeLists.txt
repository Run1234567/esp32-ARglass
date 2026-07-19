idf_component_register(SRCS "MPU6050.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c log esp_timer)
