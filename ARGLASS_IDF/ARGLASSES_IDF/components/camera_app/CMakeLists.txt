# =====================================================================
# 摄像头模块 CMakeLists.txt
# =====================================================================
# 源文件: camera_app.c
# 依赖:
#   - esp32-camera: 乐鑫摄像头驱动库
#   - log:          日志输出
#   - sd_card_app:  SD 卡挂载点 (MOUNT_POINT)
#   - my_uart:      串口通信 (通知 UI 拍照完成)
#   - driver:       GPIO 驱动
# =====================================================================

idf_component_register(SRCS "camera_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera log sd_card_app my_uart driver esp_http_client lwip esp_timer)
