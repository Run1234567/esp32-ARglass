/**
 * @file camera_app.h
 * @brief 摄像头初始化、拍照与视频推流模块公共接口
 *
 * 本模块管理 OV2640 并行摄像头，提供:
 *   - JPEG 格式拍照 (UXGA 1600x1200)
 *   - HTTP 照片上传
 *   - TCP 视频实时推流 (VGA 640x480, ~15FPS)
 */

#ifndef CAMERA_APP_H
#define CAMERA_APP_H

#include <stdbool.h>   // bool 类型定义

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

/**
 * @brief 视频推流状态标志
 * true = 正在推流，false = 停止推流
 * 由 my_uart.c 通过 CMD:VIDEO_START/STOP 控制
 */
extern volatile bool is_video_recording;

/**
 * @brief 视频推流任务
 *
 * 将摄像头 JPEG 帧通过 TCP 推送到云服务器 8890 端口。
 * 协议: [4字节长度] + [JPEG数据]，约 15 FPS。
 * 不通话时休眠待机，不占 CPU。
 */
void video_stream_task(void *pvParameters);

#endif // CAMERA_APP_H
