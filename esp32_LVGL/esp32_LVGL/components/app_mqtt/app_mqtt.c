#include "app_mqtt.h"
#include "esp_log.h"
#include <string.h>

// 必须加上的头文件
#include "cJSON.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "ui_globals.h"
#include "ui_manager.h" // 👇 新增：引入 UI 管理器以支持 ui_cmd_queue 队列

static const char *TAG = "APP_MQTT";

// 定义全局变量
esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;

// 📩 内部事件回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            is_mqtt_connected = true;
            ESP_LOGI(TAG, "✅ 主脑连接成功！");
            esp_mqtt_client_subscribe(client, "home/light/cmd", 0);
            esp_mqtt_client_publish(client, "home/status/esp32", "ONLINE", 0, 0, 0);
            esp_mqtt_client_subscribe(client, "home/status/sensor", 0);
            esp_mqtt_client_subscribe(client, "jarvis/cmd/time", 0);
            esp_mqtt_client_subscribe(client, "jarvis/glasses/display", 0);
            
            // 👇 新增：订阅网页遥控器的 MQTT 控制话题 (可根据网页实际 Topic 修改)
            esp_mqtt_client_subscribe(client, "esp32/glass/control", 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            is_mqtt_connected = false;
            ESP_LOGI(TAG, "❌ 连接断开，等待自动重连...");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "📩 收到指令: 主题=%.*s", event->topic_len, event->topic);
            
            // ==========================================
            // 1. 解析 J.A.R.V.I.S 云端下发的时间和天气数据
            // ==========================================
            if (strncmp(event->topic, "jarvis/cmd/time", event->topic_len) == 0) {
                
                // 手动给 MQTT 数据加上结束符 '\0' (防止内存溢出死机)
                char *json_str = malloc(event->data_len + 1);
                if (json_str == NULL) {
                    ESP_LOGE(TAG, "内存不足，无法解析 JSON");
                    break;
                }
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0'; 

                // 开始解析 JSON
                cJSON *root = cJSON_Parse(json_str);
                if (root != NULL) {
                    cJSON *time_item = cJSON_GetObjectItem(root, "time");
                    cJSON *date_item = cJSON_GetObjectItem(root, "date");
                    cJSON *lunar_item = cJSON_GetObjectItem(root, "lunar");

                    // 加锁更新 LVGL 屏幕
                    if (lvgl_port_lock(0)) {
                        extern lv_obj_t * label_time;
                        extern lv_obj_t * label_date;
                        extern lv_obj_t * label_lunar;

                        if (time_item && time_item->valuestring && label_time) {
                            lv_label_set_text(label_time, time_item->valuestring);
                        }
                        if (date_item && date_item->valuestring && label_date) {
                            lv_label_set_text(label_date, date_item->valuestring);
                        }
                        if (lunar_item && lunar_item->valuestring && label_lunar) {
                            lv_label_set_text(label_lunar, lunar_item->valuestring);
                        }

                        lvgl_port_unlock(); // 更新完毕，释放屏幕锁
                    }
                    cJSON_Delete(root); // 释放 JSON 对象
                } else {
                    ESP_LOGE(TAG, "JSON 解析失败!");
                }
                free(json_str); // 释放字符串内存
            }
            // ==========================================
            // 2. 解析小说下发数据
            // ==========================================
            else if (strncmp(event->topic, "jarvis/glasses/display", event->topic_len) == 0) {
                
                char *json_str = malloc(event->data_len + 1);
                if (json_str == NULL) {
                    ESP_LOGE("MQTT_RECV", "内存不足，无法接收！");
                    break;
                }
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0'; // 安全封口

                cJSON *root = cJSON_Parse(json_str);
                if (root != NULL) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
                    
                    if (cmd && cmd->valuestring && strcmp(cmd->valuestring, "novel") == 0) {
                        cJSON *data = cJSON_GetObjectItem(root, "data");
                        if (data && data->valuestring) {
                            
                            if (novel_source_buffer != NULL) {
                                free(novel_source_buffer); 
                            }
                            novel_source_buffer = strdup(data->valuestring); 
                            current_book_pos = 0; 
                            
                            ESP_LOGI("MQTT_RECV", "✅ 成功加载 %d 字节小说到本地书库！", strlen(novel_source_buffer));
                        }
                    }
                    cJSON_Delete(root); 
                }
                free(json_str); 
            }
            // ==========================================
            // 👇 3. 新增：解析 MQTT 远程控制指令（完美复刻蓝牙控制流）
            // ==========================================
            else if (strncmp(event->topic, "esp32/glass/control", event->topic_len) == 0 && event->topic_len == strlen("esp32/glass/control")) {
                
                // MQTT 传来的字符串没有 \0 结尾，必须拷贝一份并安全封口
                char *rx_data = malloc(event->data_len + 1);
                if (rx_data == NULL) {
                    ESP_LOGE(TAG, "内存不足，无法接收控制指令");
                    break;
                }
                memcpy(rx_data, event->data, event->data_len);
                rx_data[event->data_len] = '\0';

                // 去掉末尾的 \r \n 空格
                int len = strlen(rx_data);
                while (len > 0 && (rx_data[len-1] == '\r' || rx_data[len-1] == '\n' || rx_data[len-1] == ' ')) {
                    rx_data[--len] = '\0';
                }

                ESP_LOGI(TAG, "MQTT 控制数据: [%s]", rx_data);

                // 提取指令：支持纯文本 "UP" 或 JSON [{"cmd":"UP"}]
                char cmd_str[16] = {0};
                if (rx_data[0] == '[' || rx_data[0] == '{') {
                    // JSON 格式：提取 cmd 字段的值
                    cJSON *root = cJSON_Parse(rx_data);
                    if (root != NULL) {
                        cJSON *item = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : root;
                        cJSON *cmd_item = cJSON_GetObjectItem(item, "cmd");
                        if (cmd_item && cmd_item->valuestring) {
                            strncpy(cmd_str, cmd_item->valuestring, sizeof(cmd_str) - 1);
                        }
                        cJSON_Delete(root);
                    }
                } else {
                    // 纯文本格式
                    strncpy(cmd_str, rx_data, sizeof(cmd_str) - 1);
                }

                ui_cmd_t cmd = UI_CMD_NONE;

                if (strcmp(cmd_str, "UP") == 0)           cmd = UI_CMD_UP;
                else if (strcmp(cmd_str, "DOWN") == 0)    cmd = UI_CMD_DOWN;
                else if (strcmp(cmd_str, "LEFT") == 0)    cmd = UI_CMD_LEFT;
                else if (strcmp(cmd_str, "RIGHT") == 0)   cmd = UI_CMD_RIGHT;
                else if (strcmp(cmd_str, "OK") == 0)      cmd = UI_CMD_CIRCLE;

                // 如果按键有效且 UI 队列已初始化，则塞入队列传递给 UI 线程
                if (cmd != UI_CMD_NONE && ui_cmd_queue != NULL) {
                    ESP_LOGI(TAG, "🌐 MQTT 远程遥控成功: %s", rx_data);
                    xQueueSend(ui_cmd_queue, &cmd, 0);
                }

                free(rx_data); // 记得释放临时内存
            }
            break;

        default:
            break;
    }
}

// 🚀 启动函数
void app_mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://124.220.224.189:1883",
        .credentials.username = "RUN",
        .credentials.authentication.password = "88888888",
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .network.reconnect_timeout_ms = 30000, 
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// 📤 快捷发送函数
void app_mqtt_publish(const char *topic, const char *data)
{
    if (mqtt_client != NULL && is_mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, topic, data, 0, 0, 0);
    }
}