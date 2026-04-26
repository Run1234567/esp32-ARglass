#include "my_ble.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "MY_BLE";
static uint8_t own_addr_type;

// ================= 1. 定义 GATT 数据通道 =================
static const ble_uuid128_t my_service_uuid = BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16);
static const ble_uuid128_t my_char_uuid = BLE_UUID128_INIT(0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00);

static int my_ble_gatt_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            ESP_LOGI(TAG, "手机正在读取数据...");
            os_mbuf_append(ctxt->om, "JARVIS OK", 9);
            return 0;
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            ESP_LOGI(TAG, "收到手机写入数据，长度: %d", ctxt->om->om_len);
            esp_log_buffer_hex("DATA", ctxt->om->om_data, ctxt->om->om_len);
            return 0;
    }
    return 0;
}

static const struct ble_gatt_svc_def my_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &my_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &my_char_uuid.u,
                .access_cb = my_ble_gatt_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {0} 
        },
    },
    {0} 
};

// ================= 2. 核心：蓝牙事件处理与广播 =================

// 提前声明广播函数，因为回调里要用
static void ble_app_advertise(void);

// [关键修复] 处理连接和断开事件
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, ">>> 手机已成功连接！ <<<");

                // 👇👇👇 粘贴在这里：配置防 Wi-Fi 干扰的超时容忍度 👇👇👇
                struct ble_gap_upd_params params;
                params.itvl_min = 0x0060;            // 最小连接间隔 (120ms)
                params.itvl_max = 0x00C8;            // 最大连接间隔 (250ms，稍微慢一点，给WiFi让路)
                params.latency = 0;                  
                params.supervision_timeout = 0x012C; // 【核心】超时时间设为 3 秒，防止被瞬间踢下线！
                params.min_ce_len = 0;
                params.max_ce_len = 0;

                int rc = ble_gap_update_params(event->connect.conn_handle, &params);
                if (rc != 0) {
                    ESP_LOGE(TAG, "更新连接参数失败: %d", rc);
                } else {
                    ESP_LOGI(TAG, "已启用防 Wi-Fi 干扰的高容忍度模式！");
                }
                // 👆👆👆 新增代码结束 👆👆👆

            } else {
                ESP_LOGE(TAG, "连接失败，错误码: %d，恢复广播...", event->connect.status);
                ble_app_advertise(); // 失败后重新广播
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, ">>> 手机已断开，恢复广播... <<<");
            ble_app_advertise();     // 断开后重新广播，防止设备假死
            break;
    }
    return 0;
}

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播失败! 错误码: %d (可能是你的名字太长了!)", rc);
        return;
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // [关键修复] 绑定 ble_gap_event 事件回调
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败! 错误码: %d", rc);
        return;
    }
    ESP_LOGI(TAG, ">>> 蓝牙正在广播中！可以使用 nRF Connect 搜索了！ <<<");
}

static void ble_on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_app_advertise();
}

// ================= 3. 初始化入口 =================
static void ble_host_task(void *param) {
    nimble_port_run(); 
    nimble_port_freertos_deinit();
}

void my_ble_init(const char* device_name) {
    ESP_LOGI(TAG, "正在初始化 NimBLE...");
    nimble_port_init();

    if (device_name != NULL) {
        ble_svc_gap_device_name_set(device_name);
    } else {
        ble_svc_gap_device_name_set("ESP32-S3-BLE"); 
    }

    // 免密配对安全配置 (Just Works)
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; 
    ble_hs_cfg.sm_bonding = 1; 
    ble_hs_cfg.sm_mitm = 0;    
    ble_hs_cfg.sm_sc = 1;      

    // 注册 GATT 服务
    ble_svc_gatt_init();
    ble_gatts_count_cfg(my_gatt_svcs);
    ble_gatts_add_svcs(my_gatt_svcs);

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}