# ����� REQUIRES mqtt �����ڸ���ϵͳ��������Ҫ�ٷ��� mqtt �ײ��֧��
idf_component_register(SRCS "app_mqtt.c"
                       INCLUDE_DIRS "."
                       REQUIRES mqtt esp_netif cjson lvgl esp_lvgl_port LVGL_UI_mine)