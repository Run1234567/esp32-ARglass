/**
 * @file camera_app.h
 * @brief 摄像头初始化与拍照模块公共接口
 *
 * 本模块管理 OV2640 并行摄像头，提供 JPEG 格式的拍照功能。
 * 图像分辨率: UXGA (1600x1200)，帧缓冲存储在 PSRAM 中。
 */

#ifndef CAMERA_APP_H
#define CAMERA_APP_H

/**
 * @brief 初始化摄像头
 *
 * 配置 OV2640 为 JPEG/UXGA 模式，20MHz 主时钟，双缓冲。
 * 初始化后自动预热 3 帧。必须在 app_main() 中调用一次。
 */
void initCamera(void);

/**
 * @brief 拍照并保存到 SD 卡 /sdcard/PZ/ 目录
 *
 * 文件名自动递增: IMG_001.jpg, IMG_002.jpg, ...
 * 最大支持 999 张照片。
 */
void take_photo_to_PZ_folder(void);

/**
 * @brief 高分辨率拍照完整工作流
 *
 * 流程: 切换高分辨率 -> 预热 -> 拍照保存 -> 通知 UI
 * 耗时约 1 秒。拍照完成后通过 UART 发送 "CMD:PHOTO_DONE"。
 */
void execute_high_res_capture(void);

#endif // CAMERA_APP_H
