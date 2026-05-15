#include "my_ble.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "BLE_CLIENT";

// 目标服务器的名字 (必须和板子A的名字完全一样)
#define TARGET_DEVICE_NAME "My_S3_Bluetooth"

// 记录连接句柄和信箱句柄
static uint16_t peer_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t peer_chr_val_handle = 0; // 0x2222 信箱的实际操作句柄
static bool is_connected = false;

static void blecent_scan(void);

// =======================================================
// 4. 发送数据 API (供 main.c 调用)
// =======================================================
bool my_ble_send_data(const char* data) {
    if (!is_connected || peer_conn_handle == BLE_HS_CONN_HANDLE_NONE || peer_chr_val_handle == 0) {
        return false; // 没连上，或者还没找到信箱
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
    if (!om) return false;

    // 发起无响应写入 (Write Without Response)
    int rc = ble_gattc_write_no_rsp(peer_conn_handle, peer_chr_val_handle, om);
    return (rc == 0);
}

// =======================================================
// 3. 寻找信箱的回调 (发现 0x2222)
// =======================================================
static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0) {
        // 找到了信箱！记录下它的操作句柄
        peer_chr_val_handle = chr->val_handle;
        is_connected = true;
        ESP_LOGI(TAG, "🎯 成功找到 0x2222 信箱！现在可以发送数据了！");
    }
    return 0;
}

// =======================================================
// 2. 寻找大楼的回调 (发现 0x1111)
// =======================================================
static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "🏢 找到 0x1111 服务大楼，正在寻找 0x2222 信箱...");
        // 拿着大楼的范围，进去找 0x2222 信箱
        ble_uuid16_t chr_uuid = { .u.type = BLE_UUID_TYPE_16, .value = 0x2222 };
        ble_gattc_disc_chrs_by_uuid(conn_handle, service->start_handle, service->end_handle, 
                                    &chr_uuid.u, chr_disc_cb, NULL);
    }
    return 0;
}

// =======================================================
// 1. GAP 事件回调 (扫描、连接、断开)
// =======================================================
static int blecent_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        // --- 搜到新设备 ---
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            
            // 检查是不是我们要找的板子A
            if (fields.name != NULL && fields.name_len == strlen(TARGET_DEVICE_NAME)) {
                if (strncmp((char*)fields.name, TARGET_DEVICE_NAME, fields.name_len) == 0) {
                    ESP_LOGI(TAG, "👀 发现目标！停止扫描，准备连接...");
                    ble_gap_disc_cancel(); // 停止扫描
                    // 发起连接
                    ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 30000, NULL, blecent_gap_event, NULL);
                }
            }
            break;
        }
        
        // --- 连接成功或失败 ---
        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "🔗 物理连接成功！正在寻找服务...");
                peer_conn_handle = event->connect.conn_handle;
                
                // 连接成功后，马上去寻找 0x1111 大楼
                ble_uuid16_t svc_uuid = { .u.type = BLE_UUID_TYPE_16, .value = 0x1111 };
                ble_gattc_disc_svc_by_uuid(peer_conn_handle, &svc_uuid.u, svc_disc_cb, NULL);
            } else {
                ESP_LOGE(TAG, "❌ 连接失败，重新开始扫描...");
                blecent_scan();
            }
            break;
        }

        // --- 连接断开 ---
        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGW(TAG, "🥀 连接断开，重新开始扫描...");
            peer_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            peer_chr_val_handle = 0;
            is_connected = false;
            blecent_scan();
            break;
        }
        // --- 【新增】收到 Server 发来的通知 (Notify) ---
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > 0) {
                uint8_t received_data[len + 1];
                os_mbuf_copydata(event->notify_rx.om, 0, len, received_data);
                received_data[len] = '\0';
                
                // 打印出收到的消息！
                ESP_LOGI(TAG, "🔔 收到 Server 发来的消息: %s", received_data);
                
                // TODO: 这里可以把你收到的数据发给你的屏幕(LVGL)显示，或者控制灯光
            }
            break;
        }
    }
    return 0;
}

// 启动扫描
static void blecent_scan(void) {
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof disc_params);
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0;
    disc_params.window = 0;
    
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, blecent_gap_event, NULL);
    ESP_LOGI(TAG, "🕵️‍♂️ 开始扫描附近的设备...");
}

static void blecent_on_sync(void) {
    blecent_scan(); // 协议栈启动后，立刻开始扫描
}

void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// 供 main.c 调用的初始化函数
void my_ble_init(const char* device_name) {
    ESP_LOGI(TAG, "初始化 BLE 客户端模块...");
    nimble_port_init();
    ble_hs_cfg.sync_cb = blecent_on_sync;
    
    // 设置设备名称 (虽然是 Client，但也设一下)
    ble_svc_gap_device_name_set(device_name);
    
    nimble_port_freertos_init(nimble_host_task);
}