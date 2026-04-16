#include "app_mqtt.h"
#include "esp_log.h"
#include <string.h>

// 👇 必须加上的三个头文件
#include "cJSON.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "ui_globals.h"

static const char *TAG = "APP_MQTT";

// 定义全局变量
esp_mqtt_client_handle_t mqtt_client = NULL;

// 📩 内部事件回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ 主脑连接成功！");
            esp_mqtt_client_subscribe(client, "home/light/cmd", 0);
            esp_mqtt_client_publish(client, "home/status/esp32", "ONLINE", 0, 0, 0);
            
            // 💡 修复了原来的拼写错误，这里应该用局部变量 client
            esp_mqtt_client_subscribe(client, "home/status/sensor", 0);
            esp_mqtt_client_subscribe(client, "jarvis/cmd/time", 0);
            esp_mqtt_client_subscribe(client, "jarvis/glasses/display", 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "❌ 连接断开，等待自动重连...");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "📩 收到指令: 主题=%.*s", event->topic_len, event->topic);
            
            // ==========================================
            // ✨ 解析 J.A.R.V.I.S 云端下发的时间和天气数据
            // ==========================================
            if (strncmp(event->topic, "jarvis/cmd/time", event->topic_len) == 0) {
                
                // 1. 手动给 MQTT 数据加上结束符 '\0' (防止内存溢出死机)
                char *json_str = malloc(event->data_len + 1);
                if (json_str == NULL) {
                    ESP_LOGE(TAG, "内存不足，无法解析 JSON");
                    break;
                }
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0'; 

                // 2. 开始解析 JSON
                cJSON *root = cJSON_Parse(json_str);
                if (root != NULL) {
                    cJSON *time_item = cJSON_GetObjectItem(root, "time");
                    cJSON *date_item = cJSON_GetObjectItem(root, "date");
                    cJSON *lunar_item = cJSON_GetObjectItem(root, "lunar");
                    cJSON *weather_item = cJSON_GetObjectItem(root, "weather");
                    cJSON *temp_item = cJSON_GetObjectItem(root, "temp");

                    // 3. 加锁更新 LVGL 屏幕！(必须加锁，否则跨线程会死机)
                    if (lvgl_port_lock(0)) {
                        // 声明你在 ui_ar_glass.c 里的全局标签
                        extern lv_obj_t * label_time;
                        extern lv_obj_t * label_date;
                        extern lv_obj_t * label_lunar;
                        extern lv_obj_t * label_weather; 

                        if (time_item && time_item->valuestring && label_time) {
                            lv_label_set_text(label_time, time_item->valuestring);
                        }
                        if (date_item && date_item->valuestring && label_date) {
                            lv_label_set_text(label_date, date_item->valuestring);
                        }
                        if (lunar_item && lunar_item->valuestring && label_lunar) {
                            lv_label_set_text(label_lunar, lunar_item->valuestring);
                        }
                        // 组合天气 (如 "25度 晴天")
                        if (weather_item && temp_item && label_weather) {
                            char weather_str[64];
                            snprintf(weather_str, sizeof(weather_str), " %s %s", temp_item->valuestring, weather_item->valuestring);
                            lv_label_set_text(label_weather, weather_str);
                        }

                        lvgl_port_unlock(); // 更新完毕，释放屏幕锁
                    }
                    cJSON_Delete(root); // 释放 JSON 对象
                } else {
                    ESP_LOGE(TAG, "JSON 解析失败!");
                }
                free(json_str); // 释放字符串内存
            }
            else if (strncmp(event->topic, "jarvis/glasses/display", event->topic_len) == 0) {
                
                // 2. ✨ 致命防坑：MQTT 发来的数据没有 \0 结尾，必须手动拷贝并封口！
                char *json_str = malloc(event->data_len + 1);
                if (json_str == NULL) {
                    ESP_LOGE("MQTT_RECV", "内存不足，无法接收！");
                    break;
                }
                memcpy(json_str, event->data, event->data_len);
                json_str[event->data_len] = '\0'; // 安全封口

                // 3. 解析 JSON
                cJSON *root = cJSON_Parse(json_str);
                if (root != NULL) {
                    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
                    
                    // 判断是不是小说指令
                    if (cmd && cmd->valuestring && strcmp(cmd->valuestring, "novel") == 0) {
                        cJSON *data = cJSON_GetObjectItem(root, "data");
                        if (data && data->valuestring) {
                            
                            // 4. ✨ 异步交接：把数据存入书库，立刻结束网络任务
                            if (novel_source_buffer != NULL) {
                                free(novel_source_buffer); // 清空眼镜里上一段小说的内存
                            }
                            // 复制新小说到眼镜的书库缓冲区
                            novel_source_buffer = strdup(data->valuestring); 
                            current_book_pos = 0; // 书签归零，准备从头滚动
                            
                            ESP_LOGI("MQTT_RECV", "✅ 成功加载 %d 字节小说到本地书库！", strlen(novel_source_buffer));
                        }
                    }
                    cJSON_Delete(root); // 必须删除 JSON 树，防止内存泄漏
                }
                free(json_str); // 必须释放拷贝的字符串
            }
            // 如果还有其他主题的数据处理，可以继续写 else if ...
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
        .buffer.size = 2048,      // 接收缓冲区大小 (眼镜端最需要这个)
        .buffer.out_size = 2048,  // 发送缓冲区大小 (SD卡端最需要这个)
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    
}

// 📤 快捷发送函数
void app_mqtt_publish(const char *topic, const char *data)
{
    if (mqtt_client != NULL) {
        esp_mqtt_client_publish(mqtt_client, topic, data, 0, 0, 0);
    } else {
        ESP_LOGE(TAG, "MQTT 未初始化，无法发送！");
    }
}