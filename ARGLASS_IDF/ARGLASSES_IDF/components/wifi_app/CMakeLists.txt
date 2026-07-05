# =====================================================================
# WiFi STA 模块 CMakeLists.txt
# =====================================================================
# 源文件: wifi_app.c
# 依赖: esp_wifi (WiFi驱动), esp_event (事件系统), esp_netif (网络接口)
# =====================================================================

idf_component_register(SRCS "wifi_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif nvs_flash)
