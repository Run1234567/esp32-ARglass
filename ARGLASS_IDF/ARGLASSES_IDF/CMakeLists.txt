# =====================================================================
# ESP-IDF 项目根 CMakeLists.txt
# =====================================================================
# 项目名称: ARGLASSES_IDF
# 硬件平台: Seeed XIAO ESP32-S3
# 框架: ESP-IDF v5.5.0
#
# 此文件是 ESP-IDF 项目的顶层 CMake 配置，负责:
#   1. 设置 ESP-SR 语音模型的搜索路径
#   2. 引入 ESP-IDF 的 CMake 构建系统
#   3. 声明项目名称
# =====================================================================

cmake_minimum_required(VERSION 3.16.0)

# 设置 ESP-SR 语音识别模型的路径
# ESP-SR 库会在此路径下查找 WakeNet/MultiNet 等模型文件
# 目录结构: my_models/wn9_jarvis/ (唤醒词模型)
set(esp_sr_models_path "${CMAKE_SOURCE_DIR}/my_models" CACHE STRING "ESP-SR Model Path" FORCE)

# 引入 ESP-IDF 核心 CMake 构建系统
# 这个文件定义了 idf_component_register() 等关键函数
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 声明项目名称 (对应编译输出的 .bin 文件名)
project(ARGLASSES_IDF)
