# �� REQUIRES ��� nimble ��Ϊ bt
idf_component_register(SRCS "my_ble.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash bt LVGL_UI_mine my_wifi)