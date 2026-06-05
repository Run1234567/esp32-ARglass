/**
 * @file app_mqtt.c
 * @brief MQTT 客户端模块 (遗留/未使用)
 *
 * =====================================================================
 * ⚠️ 注意：此模块为遗留代码，当前版本已不再使用！
 * =====================================================================
 * 原本用于通过 MQTT 协议与服务器通信，但已被 WebSocket 方案替代。
 * main.c 中不再包含 app_mqtt.h，所有通信改用 esp_websocket_client。
 *
 * 保留此文件的原因：
 *   - 作为 MQTT 实现的参考
 *   - 未来可能用于特定场景 (如 IoT 设备控制)
 *
 * MQTT 服务器配置：
 *   - 地址: mqtt://124.220.224.189:1883
 *   - 用户名: RUN
 *   - 密码: 88888888
 *
 * 订阅主题：
 *   - home/light/cmd:     灯光控制命令
 *   - jarvis/glasses/book: 小说阅读命令
 *
 * 依赖组件：
 *   - mqtt:     ESP-IDF MQTT 客户端库
 *   - esp_netif: 网络接口
 */

#include "app_mqtt.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_MQTT";  // 日志标签

/* =====================================================================
 * 全局变量
 * ===================================================================== */
esp_mqtt_client_handle_t mqtt_client = NULL;  // MQTT 客户端句柄

/**
 * @brief 小说翻页信号量 (遗留)
 * 原本用于 MQTT 接收小说命令后触发翻页
 * 现在已移到 main.c 中，由 WebSocket/UART 触发
 */
SemaphoreHandle_t next_page_sem = NULL;

/* =====================================================================
 * MQTT 事件回调函数
 * =====================================================================
 * @brief 处理 MQTT 连接/断开/数据接收等事件
 *
 * @param handler_args 用户参数 (未使用)
 * @param base         事件基类
 * @param event_id     事件 ID
 * @param event_data   事件数据
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            /* ---- 连接成功 ---- */
            ESP_LOGI(TAG, "🔗 主脑连接成功！");
            // 订阅控制主题
            esp_mqtt_client_subscribe(client, "home/light/cmd", 0);      // QoS 0
            esp_mqtt_client_subscribe(client, "jarvis/glasses/book", 0); // QoS 0
            break;

        case MQTT_EVENT_DISCONNECTED:
            /* ---- 连接断开 ---- */
            ESP_LOGI(TAG, "🔌 连接断开，等待自动重连...");
            // ESP-IDF MQTT 客户端会自动尝试重连
            break;

        case MQTT_EVENT_DATA:
            /* ---- 收到数据 ---- */
            ESP_LOGI(TAG, "📩 收到指令: 主题=%.*s | 内容=%.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);
            // TODO: 根据主题和内容执行相应操作
            break;

        default:
            break;
    }
}

/* =====================================================================
 * MQTT 启动函数
 * =====================================================================
 * @brief 初始化并启动 MQTT 客户端
 *
 * 配置：
 *   - Broker: mqtt://124.220.224.189:1883
 *   - 认证: 用户名/密码
 *   - 缓冲区: 收发各 2048 字节
 */
void app_mqtt_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://124.220.224.189:1883",  // MQTT Broker 地址
        .credentials.username = "RUN",                          // 用户名
        .credentials.authentication.password = "88888888",      // 密码
        .buffer.size = 2048,       // 接收缓冲区大小
        .buffer.out_size = 2048,   // 发送缓冲区大小
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* =====================================================================
 * MQTT 数据发送函数
 * =====================================================================
 * @brief 向指定主题发布消息
 *
 * @param topic MQTT 主题
 * @param data  消息内容
 */
void app_mqtt_publish(const char *topic, const char *data) {
    if (mqtt_client != NULL) {
        esp_mqtt_client_publish(mqtt_client, topic, data, 0, 0, 0);
        // 参数: 客户端, 主题, 数据, 长度(0=自动), QoS, retain
    } else {
        ESP_LOGE(TAG, "MQTT 未初始化，无法发送！");
    }
}
