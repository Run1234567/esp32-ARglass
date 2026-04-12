idf_component_register(SRCS "my_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi nvs_flash esp_event esp_netif lwip)