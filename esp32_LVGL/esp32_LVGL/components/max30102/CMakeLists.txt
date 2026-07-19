file(GLOB_RECURSE srcs "*.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c freertos esp_timer)
