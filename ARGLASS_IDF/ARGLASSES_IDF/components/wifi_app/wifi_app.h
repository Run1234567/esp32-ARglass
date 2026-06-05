/**
 * @file wifi_app.h
 * @brief WiFi STA 模块公共接口头文件
 *
 * 本模块提供 WiFi Station (客户端) 模式的初始化功能。
 * 调用 wifi_init_sta() 后，ESP32-S3 将自动连接到预配置的热点。
 */

#ifndef WIFI_APP_H
#define WIFI_APP_H

/**
 * @brief 初始化 WiFi 为 Station 模式并开始连接
 *
 * 此函数会：
 *   1. 初始化 TCP/IP 协议栈和事件循环
 *   2. 注册 WiFi/IP 事件回调
 *   3. 配置 SSID/密码并启动 WiFi 驱动
 *   4. 自动连接，失败最多重试 10 次
 *
 * 注意：此函数是非阻塞的，它只启动连接流程，
 * 实际连接完成会通过事件回调通知。
 * 调用后需要等待一段时间 (约 3-5 秒) 才能获得 IP 地址。
 */
void wifi_init_sta(void);

#endif // WIFI_APP_H
