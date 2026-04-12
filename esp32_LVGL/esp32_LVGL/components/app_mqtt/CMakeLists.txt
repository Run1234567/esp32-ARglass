# 这里的 REQUIRES mqtt 就是在告诉系统：我们需要官方的 mqtt 底层库支持
idf_component_register(SRCS "app_mqtt.c"
                       INCLUDE_DIRS "."
                       REQUIRES mqtt esp_netif)