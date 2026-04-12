#include "app_mqtt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_MQTT";

// 定义全局变量
esp_mqtt_client_handle_t mqtt_client = NULL;

// ? 内部事件回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "? 主脑连接成功！");
            esp_mqtt_client_subscribe(client, "home/light/cmd", 0);
            esp_mqtt_client_publish(client, "home/status/esp32", "ONLINE", 0, 0, 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "? 连接断开，等待自动重连...");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "? 收到指令: 主题=%.*s | 内容=%.*s", 
                     event->topic_len, event->topic, 
                     event->data_len, event->data);
            break;

        default:
            break;
    }
}

// ? 启动函数
void app_mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://124.220.224.189:1883",
        .credentials.username = "RUN",
        .credentials.authentication.password = "88888888",
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ? 快捷发送函数
void app_mqtt_publish(const char *topic, const char *data)
{
    if (mqtt_client != NULL) {
        esp_mqtt_client_publish(mqtt_client, topic, data, 0, 0, 0);
    } else {
        ESP_LOGE(TAG, "MQTT 未初始化，无法发送！");
    }
}