# =====================================================================
# 录音与拍照模块 CMakeLists.txt
# =====================================================================
# 源文件: record_app.c
# 依赖:
#   - sd_card_app:  SD 卡挂载点 (MOUNT_POINT)
#   - audio_app:    麦克风接口
#   - esp32-camera: 摄像头接口
#   - my_uart:      串口通信 (发送文件列表给 UI)
#   - esp_http_client: HTTP客户端 (上传文件)
#   - esp_ringbuf:  环形缓冲区 (音频数据)
# =====================================================================
# 注意：不依赖esp_netif和esp_event，使用轮询方式检测网络状态
# 这样可以完全兼容PlatformIO和所有ESP-IDF版本
# =====================================================================

idf_component_register(SRCS "record_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES sd_card_app audio_app esp32-camera my_uart esp_http_client esp_ringbuf)
