idf_component_register(SRCS "tft_display.c"
                       INCLUDE_DIRS "."      # <-- 把 "include" 改成 "." (一个英文句号，代表当前目录)
                       REQUIRES esp_lcd driver)