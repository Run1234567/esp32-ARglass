#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "mqtt_client.h"

// ��¶��ȫ�ֿͻ��˾��
extern esp_mqtt_client_handle_t mqtt_client;
extern bool is_mqtt_connected;

// 初始化并启动 MQTT
void app_mqtt_start(void);

// ��װһ����ݷ��ͺ���
void app_mqtt_publish(const char *topic, const char *data);

#endif // APP_MQTT_H