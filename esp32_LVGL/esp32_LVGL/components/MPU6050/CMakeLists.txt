# 注册组件，定义源文件和头文件目录
idf_component_register(SRCS "MPU6050.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver log esp_timer)