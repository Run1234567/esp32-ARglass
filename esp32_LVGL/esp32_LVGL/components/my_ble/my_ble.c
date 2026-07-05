#include "my_ble.h"
#include "my_uart.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "my_wifi.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ui_manager.h" // 你的 UI 管理器

static const char *TAG = "BLE_DUAL";

// ==================== 全局变量 ====================
// --- 魔杖端 (Client) ---
#define TARGET_DEVICE_NAME "Cyberry_Wand"
static uint16_t wand_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t wand_chr_val_handle = 0; // 0x2222 信箱操作句柄
static bool is_wand_connected = false;

// --- 网页端 (Server) ---
static uint16_t web_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t web_char_val_handle = 0;

static void blecent_scan(void);
static void ble_app_advertise(void);
static int blecent_gap_event(struct ble_gap_event *event, void *arg);

// =======================================================
// 向网页端发送数据（Notify）
// =======================================================
bool my_ble_send_to_web(const char* data) {
    if (web_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
    int rc = ble_gatts_notify_custom(web_conn_handle, web_char_val_handle, om);
    return (rc == 0);
}

// =======================================================
// 【新增】网页端：接收 HTML 数据的回调 (0x1112 -> 0x4444)
// =======================================================
static int web_gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0) {
            uint8_t rx_data[len + 1];
            os_mbuf_copydata(ctxt->om, 0, len, rx_data);
            rx_data[len] = '\0';
            ESP_LOGI(TAG, "收到网页指令: %s", rx_data);

            // 处理 SCAN_WIFI 指令
            if (strcmp((char*)rx_data, "SCAN_WIFI") == 0) {
                ESP_LOGI(TAG, "启动 Wi-Fi 扫描...");
                extern void ui_wifi_scan_start(void);
                ui_wifi_scan_start();
            }
            // 处理 JSON 格式的 WiFi 配网指令
            else if (rx_data[0] == '{') {
                cJSON *root = cJSON_Parse((char*)rx_data);
                if (root != NULL) {
                    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
                    cJSON *pwd_item = cJSON_GetObjectItem(root, "pwd");
                    if (ssid_item && pwd_item) {
                        const char *ssid = ssid_item->valuestring;
                        const char *pwd = pwd_item->valuestring;
                        ESP_LOGI(TAG, "配网: SSID=%s", ssid);
                        save_wifi_to_nvs(ssid, pwd);
                        my_wifi_connect_from_ble(ssid, pwd);
                        my_ble_send_to_web("{\"status\":\"connecting\"}");

                        // 通过串口转发给核心板
                        char uart_buf[128];
                        snprintf(uart_buf, sizeof(uart_buf), "WIFI:%s,%s\r\n", ssid, pwd);
                        my_uart_send(uart_buf);
                        ESP_LOGI(TAG, "串口转发: %s", uart_buf);
                    }
                    cJSON_Delete(root);
                }
            }
            // 网页遥控器方向键和确认键
            else {
                ui_cmd_t cmd = UI_CMD_NONE;

                if (strcmp((char*)rx_data, "UP") == 0)           cmd = UI_CMD_UP;
                else if (strcmp((char*)rx_data, "DOWN") == 0)    cmd = UI_CMD_DOWN;
                else if (strcmp((char*)rx_data, "LEFT") == 0)    cmd = UI_CMD_LEFT;
                else if (strcmp((char*)rx_data, "RIGHT") == 0)   cmd = UI_CMD_RIGHT;
                else if (strcmp((char*)rx_data, "OK") == 0)      cmd = UI_CMD_CIRCLE;
                // 全局屏幕跳转
                else if (strcmp((char*)rx_data, "GOTO_AR") == 0)        cmd = UI_CMD_GOTO_AR;
                else if (strcmp((char*)rx_data, "GOTO_MENU") == 0)      cmd = UI_CMD_GOTO_MENU;
                else if (strcmp((char*)rx_data, "GOTO_NOVEL") == 0)     cmd = UI_CMD_GOTO_NOVEL;
                else if (strcmp((char*)rx_data, "GOTO_CLOCK") == 0)     cmd = UI_CMD_GOTO_CLOCK;
                else if (strcmp((char*)rx_data, "GOTO_RECORD") == 0)    cmd = UI_CMD_GOTO_RECORD;
                else if (strcmp((char*)rx_data, "GOTO_PLAYLIST") == 0)  cmd = UI_CMD_GOTO_PLAYLIST;
                else if (strcmp((char*)rx_data, "GOTO_CAMERA") == 0)    cmd = UI_CMD_GOTO_CAMERA;
                else if (strcmp((char*)rx_data, "GOTO_NOISE") == 0)     cmd = UI_CMD_GOTO_NOISE;
                else if (strcmp((char*)rx_data, "GOTO_PITCH") == 0)     cmd = UI_CMD_GOTO_PITCH;
                else if (strcmp((char*)rx_data, "GOTO_MUSIC") == 0)     cmd = UI_CMD_GOTO_MUSIC;
                else if (strcmp((char*)rx_data, "GOTO_LIGHT") == 0)     cmd = UI_CMD_GOTO_LIGHT;
                else if (strcmp((char*)rx_data, "GOTO_HEALTH") == 0)    cmd = UI_CMD_GOTO_HEALTH;
                else if (strcmp((char*)rx_data, "GOTO_WIFI") == 0)      cmd = UI_CMD_GOTO_WIFI;
                else if (strcmp((char*)rx_data, "GOTO_GPS") == 0)       cmd = UI_CMD_GOTO_GPS;
                else if (strcmp((char*)rx_data, "GOTO_AI") == 0)        cmd = UI_CMD_GOTO_AI;
                else if (strcmp((char*)rx_data, "GOTO_CALL") == 0)      cmd = UI_CMD_GOTO_CALL;
                else if (strcmp((char*)rx_data, "GOTO_AUDIO") == 0)     cmd = UI_CMD_GOTO_AUDIO;
                else if (strcmp((char*)rx_data, "GOTO_GAME_LIST") == 0) cmd = UI_CMD_GOTO_GAME_LIST;

                if (cmd != UI_CMD_NONE && ui_cmd_queue != NULL) {
                    ESP_LOGI(TAG, "网页遥控: %s", rx_data);
                    xQueueSend(ui_cmd_queue, &cmd, 0);
                }
            }
        }
    }
    return 0;
}

// 网页端的服务表定义
static const struct ble_gatt_svc_def web_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1112),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x4444),
                .access_cb = web_gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &web_char_val_handle
            },
            { 0 }
        }
    },
    { 0 }
};

// =======================================================
// 【新增】网页端：处理网页连接的 GAP 事件
// =======================================================
static int web_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "🔗 HTML 网页已成功连接！");
                web_conn_handle = event->connect.conn_handle;
            } else {
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "❌ HTML 网页连接断开，重新开启广播...");
            web_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ble_app_advertise();
            break;
    }
    return 0;
}

// =======================================================
// 【保留】魔杖端：发送数据 API (供 main.c 调用，给魔杖发数据)
// =======================================================
bool my_ble_send_data(const char* data) {
    if (!is_wand_connected || wand_conn_handle == BLE_HS_CONN_HANDLE_NONE || wand_chr_val_handle == 0) {
        return false; 
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
    if (!om) return false;
    int rc = ble_gattc_write_no_rsp(wand_conn_handle, wand_chr_val_handle, om);
    return (rc == 0);
}

// =======================================================
// 【保留】魔杖端：寻找信箱的回调
// =======================================================
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0) {
        wand_chr_val_handle = chr->val_handle;
        is_wand_connected = true;
        ESP_LOGI(TAG, "✅ 成功找到 0x2222 信箱！可以给魔杖发送数据了！");
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "🏢 找到 0x1111 服务大楼，正在寻找 0x3333 信箱...");
        ble_uuid16_t chr_uuid = { .u.type = BLE_UUID_TYPE_16, .value = 0x3333 };
        ble_gattc_disc_chrs_by_uuid(conn_handle, service->start_handle, service->end_handle, 
                                    &chr_uuid.u, chr_disc_cb, NULL);
    }
    return 0;
}

// =======================================================
// 【保留】魔杖端：处理魔杖连接的 GAP 事件
// =======================================================
static int blecent_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            
            if (fields.name != NULL && fields.name_len == strlen(TARGET_DEVICE_NAME)) {
                if (strncmp((char*)fields.name, TARGET_DEVICE_NAME, fields.name_len) == 0) {
                    ESP_LOGI(TAG, "🎯 发现魔杖！停止扫描，准备连接...");
                    ble_gap_disc_cancel(); 
                    // ⚠️ 注意：连接成功后继续使用 blecent_gap_event 处理魔杖的事件
                    ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 30000, NULL, blecent_gap_event, NULL);
                }
            }
            break;
        }
        
        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "🔌 物理连接魔杖成功！正在寻找服务...");
                wand_conn_handle = event->connect.conn_handle;
                
                ble_uuid16_t svc_uuid = { .u.type = BLE_UUID_TYPE_16, .value = 0x1111 };
                ble_gattc_disc_svc_by_uuid(wand_conn_handle, &svc_uuid.u, svc_disc_cb, NULL);
            } else {
                ESP_LOGE(TAG, "⚠️ 连接魔杖失败，重新开始扫描...");
                blecent_scan();
            }
            break;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGW(TAG, "⚠️ 与魔杖连接断开，重新开始扫描...");
            wand_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            wand_chr_val_handle = 0;
            is_wand_connected = false;
            blecent_scan();
            break;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > 0) {
                uint8_t received_data[len + 1];
                os_mbuf_copydata(event->notify_rx.om, 0, len, received_data);
                received_data[len] = '\0';

                ESP_LOGI(TAG, "🪄 收到魔杖指令: %s", received_data);

                ui_cmd_t cmd = UI_CMD_NONE;
                if (strstr((char*)received_data, "SwipeUp") != NULL) cmd = UI_CMD_UP;
                else if (strstr((char*)received_data, "SwipeDown") != NULL) cmd = UI_CMD_DOWN;
                else if (strstr((char*)received_data, "SwipeRight") != NULL) cmd = UI_CMD_RIGHT;
                else if (strstr((char*)received_data, "SwipeLeft") != NULL) cmd = UI_CMD_LEFT;
                else if (strstr((char*)received_data, "Circle") != NULL) cmd = UI_CMD_CIRCLE;

                if (cmd != UI_CMD_NONE && ui_cmd_queue != NULL) {
                    xQueueSend(ui_cmd_queue, &cmd, 0); 
                }
            }
            break;
        }
    }
    return 0;
}

// =======================================================
// 【功能启动区】
// =======================================================
static void blecent_scan(void) {
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof disc_params);
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    
    // 启动扫描，用 blecent_gap_event 接收扫描结果
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, blecent_gap_event, NULL);
    ESP_LOGI(TAG, "🕵️‍♂️ 开始扫描附近的魔杖...");
}

static void ble_app_advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"ESP_Web"; // HTML网页去搜这个名字
    fields.name_len = strlen("ESP_Web");
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    // 开启广播，用单独的 web_gap_event 来处理网页的连接，彻底和魔杖事件剥离！
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, web_gap_event, NULL);
    ESP_LOGI(TAG, "📡 专属网页通道广播已开启，等待 HTML 连接...");
}

static void ble_on_sync(void) {
    // 协议栈就绪后，两件事同时干！
    blecent_scan();      // 1. 去搜物理魔杖
    ble_app_advertise(); // 2. 开启广播等网页连
}

static void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// 供 main.c 调用的初始化函数
void my_ble_init(const char* device_name) {
    ESP_LOGI(TAG, "🚀 初始化双模 BLE (Client连魔杖 + Server等网页)...");
    nimble_port_init();
    
    // 初始化网页端需要的服务
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(web_svr_svcs);
    ble_gatts_add_svcs(web_svr_svcs);
    
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_device_name_set("ESP_Web"); 
    
    nimble_port_freertos_init(nimble_host_task);
}