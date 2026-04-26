idf_component_register(SRCS "main.c" "my_font_cn_16.c"
                       INCLUDE_DIRS "."
                       REQUIRES tft_display MPU6050 MAX30105 my_wifi audio_driver esp_websocket_client app_mqtt LVGL_UI_mine bmp280 my_ble
                    )