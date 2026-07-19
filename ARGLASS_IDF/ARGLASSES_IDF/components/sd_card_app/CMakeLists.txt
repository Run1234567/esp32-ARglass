# =====================================================================
# SD 卡管理模块 CMakeLists.txt
# =====================================================================
# 源文件: sd_card_app.c
# 依赖:
#   - fatfs:     FAT 文件系统
#   - sdmmc:     SDMMC 主机驱动
#   - vfs:       虚拟文件系统
#   - driver:    GPIO 驱动
#   - log:       日志输出
#   - tts_app:   TTS 引擎 (小说阅读时调用)
#   - my_uart:   串口通信 (发送数据给 UI)
# =====================================================================

idf_component_register(SRCS "sd_card_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sdmmc vfs esp_driver_sdmmc esp_driver_gpio log tts_app my_uart)
