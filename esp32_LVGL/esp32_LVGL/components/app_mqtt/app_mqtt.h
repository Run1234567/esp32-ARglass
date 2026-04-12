#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "mqtt_client.h"

// 暴露出全局客户端句柄
extern esp_mqtt_client_handle_t mqtt_client;

// 初始化并启动 MQTT
void app_mqtt_start(void);

// 封装一个快捷发送函数
void app_mqtt_publish(const char *topic, const char *data);

#endif // APP_MQTT_H