# components/LVGL_UI_mine/CMakeLists.txt

# 可以用通配符把该目录下所有的 .c 文件都加进来，以后新建屏幕就不用改这里了
file(GLOB_RECURSE srcs "*.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES lvgl esp_lvgl_port app_mqtt)