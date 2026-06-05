/**
 * @file wifi_app.c
 * @brief WiFi STA (Station) 模式连接模块
 *
 * =====================================================================
 * 模块功能：
 * =====================================================================
 * 将 ESP32-S3 作为 WiFi 客户端 (Station 模式) 连接到指定的无线热点。
 * 连接成功后，设备将获得 IP 地址，可以进行 WebSocket/MQTT 等网络通信。
 *
 * 连接流程：
 *   1. 初始化 TCP/IP 协议栈和事件循环
 *   2. 注册 WiFi 和 IP 事件回调
 *   3. 配置 SSID/密码/认证模式
 *   4. 启动 WiFi 驱动
 *   5. 自动连接，失败最多重试 10 次
 *
 * 依赖组件：
 *   - esp_wifi:   WiFi 驱动
 *   - esp_event:  事件循环系统
 *   - esp_netif:  网络接口抽象层
 */

#include "wifi_app.h"

#include <stdio.h>
#include <string.h>

/* ==================== FreeRTOS 头文件 ==================== */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"  // 事件组 (可用于同步等待连接，但本模块未使用)

/* ==================== ESP-IDF 网络头文件 ==================== */
#include "esp_system.h"   // 系统级 API
#include "esp_wifi.h"      // WiFi 驱动核心 API
#include "esp_event.h"     // 事件循环 (注册回调)
#include "esp_log.h"       // 日志输出
#include "esp_netif.h"     // 网络接口 (创建 STA 接口)

/* =====================================================================
 * WiFi 配置参数
 * =====================================================================
 * TODO: 生产环境中应将 SSID 和密码存储在 NVS 或通过 BLE 配网
 */
#define WIFI_SSID      "RUN"        // 要连接的 WiFi 热点名称
#define WIFI_PASS      "88888888"   // WiFi 密码
#define MAXIMUM_RETRY  10           // 最大重试次数，超过后放弃连接

static const char *TAG = "WIFI_TEST";  // 日志标签
static int s_retry_num = 0;            // 当前已重试次数

/* =====================================================================
 * WiFi 事件回调函数 ("接线员")
 * =====================================================================
 * @brief 处理 WiFi 和 IP 相关的系统事件
 *
 * 事件说明：
 *   - WIFI_EVENT_STA_START:       WiFi 驱动启动完成，开始连接 AP
 *   - WIFI_EVENT_STA_DISCONNECTED: 连接断开，尝试重连 (最多 MAXIMUM_RETRY 次)
 *   - IP_EVENT_STA_GOT_IP:        成功获取 IP 地址
 *
 * @param arg        用户参数 (未使用)
 * @param event_base 事件基类 (WIFI_EVENT 或 IP_EVENT)
 * @param event_id   事件 ID
 * @param event_data 事件数据指针
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {

    /* ---- WiFi 驱动启动完成 ---- */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi Started. Connecting to AP...");
        esp_wifi_connect();  // 开始连接热点
    }

    /* ---- WiFi 连接断开 ---- */
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();  // 自动重连
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected. Retrying connection (%d)...", s_retry_num);
        } else {
            // 超过最大重试次数，放弃连接
            ESP_LOGE(TAG, "Connection failed! Exceeded maximum retries.");
            // TODO: 可在此处进入低功耗模式或启动 BLE 配网
        }
    }

    /* ---- 成功获取 IP 地址 ---- */
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi Connected Successfully!");
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;  // 重置重试计数器
        // TODO: 可在此处触发 WebSocket 连接等后续操作
    }
}

/* =====================================================================
 * WiFi 初始化函数 (STA 模式)
 * =====================================================================
 * @brief 初始化 WiFi 为 Station (客户端) 模式并开始连接
 *
 * 初始化步骤：
 *   1. esp_netif_init()              - 初始化 TCP/IP 协议栈
 *   2. esp_event_loop_create_default() - 创建默认事件循环
 *   3. esp_netif_create_default_wifi_sta() - 创建默认 STA 网络接口
 *   4. esp_wifi_init()               - 初始化 WiFi 驱动
 *   5. 注册事件回调                   - 监听连接/断开/IP获取等事件
 *   6. 配置 WiFi 参数                - SSID、密码、认证模式
 *   7. esp_wifi_start()              - 启动 WiFi 驱动
 *
 * 认证模式: WPA2-PSK (最常用的家用 WiFi 加密方式)
 * PMF (Protected Management Frames): 可选启用，增强安全性
 */
void wifi_init_sta(void) {
    /* ---- 步骤1: 初始化网络和事件系统 ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();  // 创建 STA 接口 (wlan0)

    /* ---- 步骤2: 初始化 WiFi 驱动 ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // 使用默认配置
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ---- 步骤3: 注册事件回调 ---- */
    esp_event_handler_instance_t instance_any_id;   // WiFi 事件实例
    esp_event_handler_instance_t instance_got_ip;   // IP 事件实例

    // 监听所有 WiFi 事件 (STA_START, STA_DISCONNECTED, etc.)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));

    // 监听 IP 获取事件
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    /* ---- 步骤4: 配置 WiFi 参数 ---- */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,        // 目标热点名称
            .password = WIFI_PASS,    // 热点密码
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // 最低认证模式: WPA2
            .pmf_cfg = {
                .capable = true,      // 支持 PMF (受保护的管理帧)
                .required = false     // 不强制要求 PMF (兼容性更好)
            },
        },
    };

    /* ---- 步骤5: 设置模式、配置、启动 ---- */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));               // 设置为 STA 模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); // 应用配置
    ESP_ERROR_CHECK(esp_wifi_start());                                // 启动 WiFi 驱动

    ESP_LOGI(TAG, "wifi_init_sta finished. Waiting for events...");
}
