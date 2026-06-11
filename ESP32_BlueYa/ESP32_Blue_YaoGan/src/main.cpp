#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

// --- 蓝牙 NimBLE 库 ---
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// --- ADC 单次采样库 (ESP-IDF v5) ---
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "CYBER_WAND";
#define DEVICE_NAME "Cyberry_Wand"

// ==========================================================
// 1. 蓝牙 GATT 服务与变量配置
// ==========================================================
static uint16_t notify_chr_val_handle;
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static void start_advertising(void);

// 发送字符串数据给网页端
void server_send_data(const char* msg) {
    uint16_t conn_handle = current_conn_handle;
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
        if (om != NULL) {
            ble_gatts_notify_custom(conn_handle, notify_chr_val_handle, om);
        }
    }
}

static int gatt_svr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    return 0;
}

static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0x1111);
static const ble_uuid16_t chr_uuid = BLE_UUID16_INIT(0x3333);

static const struct ble_gatt_chr_def gatt_svr_chrs[] = {
    {
        .uuid = &chr_uuid.u,
        .access_cb = gatt_svr_access,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
        .val_handle = &notify_chr_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .includes = NULL,
        .characteristics = gatt_svr_chrs
    },
    { 0 }
};

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            current_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "📱 网页端已连接！");
        } else {
            start_advertising();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "💔 连接断开，重新开始广播...");
        start_advertising();
    }
    return 0;
}

static void start_advertising(void) {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, ble_gap_event, NULL);
}

static void ble_on_sync(void) {
    start_advertising();
}

void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ==========================================================
// 2. 摇杆读取与方向判定逻辑 (ESP32-S3 ADC1 无冲突模式)
// ==========================================================
typedef enum {
    STATE_CENTER,
    STATE_UP,       // X减小
    STATE_DOWN,     // X增大
    STATE_LEFT,     // Y减小
    STATE_RIGHT     // Y增大
} JoystickState;

void joystick_ble_task(void *pvParameters) {
    // 1. 初始化 ADC1 单元 (彻底避开蓝牙对 ADC2 的占用)
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1, // 明确使用 ADC1
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    // 2. 在 ESP32-S3 上，GPIO 8 和 9 原生就是 ADC1 的通道 7 和 8
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_7, &config)); // GPIO 8 (X轴)
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_8, &config)); // GPIO 9 (Y轴)

    int raw_x = 0;
    int raw_y = 0;
    int log_counter = 0; 
    JoystickState current_state = STATE_CENTER;

    ESP_LOGI(TAG, "🎮 摇杆引擎启动 (ADC1 无冲突模式)，等待操作...");

    while (1) {
        // 由于使用的是 ADC1，即使蓝牙在工作，这里也可以放心使用 ESP_ERROR_CHECK
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_7, &raw_x)); // 读 GPIO 8
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_8, &raw_y)); // 读 GPIO 9

        if (++log_counter >= 10) {
            ESP_LOGI(TAG, "ADC精准值 -> X: %4d | Y: %4d", raw_x, raw_y);
            log_counter = 0;
        }

        JoystickState new_state = STATE_CENTER;

        // X减小=上, X增大=下, Y减小=左, Y增大=右
        if (raw_x < 1000) {
            new_state = STATE_UP;
        } else if (raw_x > 3000) {
            new_state = STATE_DOWN;
        } else if (raw_y < 1000) {
            new_state = STATE_LEFT;
        } else if (raw_y > 3000) {
            new_state = STATE_RIGHT;
        }

        if (new_state != current_state) {
            current_state = new_state;

            if (current_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                if (new_state == STATE_UP) {
                    server_send_data("SwipeUp");
                    ESP_LOGI(TAG, "发射 -> SwipeUp");
                } else if (new_state == STATE_DOWN) {
                    server_send_data("SwipeDown");
                    ESP_LOGI(TAG, "发射 -> SwipeDown");
                } else if (new_state == STATE_LEFT) {
                    server_send_data("SwipeLeft");
                    ESP_LOGI(TAG, "发射 -> SwipeLeft");
                } else if (new_state == STATE_RIGHT) {
                    server_send_data("SwipeRight");
                    ESP_LOGI(TAG, "发射 -> SwipeRight");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==========================================================
// 3. 主函数
// ==========================================================
#ifdef __cplusplus
extern "C" {
#endif
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    xTaskCreate(joystick_ble_task, "joystick_task", 4096, NULL, 5, NULL);
}
#ifdef __cplusplus
}
#endif