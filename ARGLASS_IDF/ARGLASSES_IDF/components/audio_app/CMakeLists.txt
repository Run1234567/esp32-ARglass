idf_component_register(
    SRCS "audio_app.c"
    INCLUDE_DIRS "."         # 头文件在当前文件夹
    REQUIRES log driver      # 声明依赖日志库和底层硬件驱动库(driver)
)