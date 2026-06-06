# components/LVGL_UI_mine/CMakeLists.txt
# updated: include img_XSbird.c img_tree.c
file(GLOB_RECURSE srcs "*.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl esp_lvgl_port app_mqtt light_sensor max30102)