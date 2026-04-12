idf_component_register(
    # 1. SRCS: 告诉编译器有哪些 .c 源文件需要编译
    SRCS "audio_driver.c"

    # 2. INCLUDE_DIRS: 告诉编译器去哪里找对外的 .h 头文件 (相对路径)
    INCLUDE_DIRS "."

    # 3. REQUIRES / PRIV_REQUIRES: 声明依赖 (关键！)
    # 你的 audio_driver 用到了 driver/i2s_std.h 和 driver/gpio.h
    # 它们属于 ESP-IDF 内部的 "driver" 组件，所以必须在这里声明依赖
    REQUIRES driver
)