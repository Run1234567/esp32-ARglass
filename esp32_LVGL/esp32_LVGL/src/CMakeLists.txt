idf_component_register(SRCS "main.cpp" "my_font_cn_16.c"
                       INCLUDE_DIRS "."
                       REQUIRES tft_display MPU6050 my_wifi esp_websocket_client app_mqtt LVGL_UI_mine bmp280 my_ble max30102 paj7620 encoder motor_pwm driver esp_driver_gpio esp_driver_i2c esp_driver_uart
                    )