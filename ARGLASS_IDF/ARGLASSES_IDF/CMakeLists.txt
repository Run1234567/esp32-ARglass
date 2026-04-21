cmake_minimum_required(VERSION 3.16.0)
set(esp_sr_models_path "${CMAKE_SOURCE_DIR}/my_models" CACHE STRING "ESP-SR Model Path" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ARGLASSES_IDF)
