cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_LVGL)

# 移除 ESP-IDF 默认的 -Werror
idf_build_replace_option_from_property(COMPILE_OPTIONS "-Werror" "")
idf_build_replace_option_from_property(COMPILE_OPTIONS "-Werror=all" "")
