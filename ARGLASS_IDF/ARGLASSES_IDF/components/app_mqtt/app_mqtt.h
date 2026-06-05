/**
 * @file app_mqtt.h
 * @brief MQTT 客户端模块公共接口 (遗留/未使用)
 *
 * ⚠️ 此模块为遗留代码，已被 WebSocket 替代。
 * main.c 中不再使用此模块。
 */

#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "mqtt_client.h"  // ESP-IDF MQTT 客户端库

/**
 * @brief MQTT 客户端句柄 (遗留)
 */
extern esp_mqtt_client_handle_t mqtt_client;

/**
 * @brief 小说翻页信号量 (遗留)
 * 已移到 main.c 中管理
 */
extern SemaphoreHandle_t next_page_sem;

/**
 * @brief 初始化并启动 MQTT 客户端 (遗留)
 * 连接到 mqtt://124.220.224.189:1883
 */
void app_mqtt_start(void);

/**
 * @brief 发布 MQTT 消息 (遗留)
 * @param topic MQTT 主题
 * @param data  消息内容
 */
void app_mqtt_publish(const char *topic, const char *data);

#endif // APP_MQTT_H
