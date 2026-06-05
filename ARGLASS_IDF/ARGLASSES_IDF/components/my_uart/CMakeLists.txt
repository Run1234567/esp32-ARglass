# =====================================================================
# UART 命令接口模块 CMakeLists.txt
# =====================================================================
# 源文件: my_uart.c
# 依赖:
#   - driver:      UART 驱动
#   - sd_card_app:  SD 卡模块 (书籍/音乐列表扫描)
#   - record_app:  录音模块 (录音控制)
#   - music_app:   音乐播放模块 (播放控制)
# =====================================================================

idf_component_register(SRCS "my_uart.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver sd_card_app record_app music_app)
