idf_component_register(SRCS "bmp280.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lvgl_port LVGL_UI_mine)