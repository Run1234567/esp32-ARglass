#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- NimBLE 蓝牙协议栈 ---
#include "nvs_flash.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "TRACKBALL";

// ==========================================================
// 1. 轨迹球硬件引脚定义
// ==========================================================
#define TB_PIN_UP     GPIO_NUM_4
#define TB_PIN_DOWN   GPIO_NUM_5
#define TB_PIN_LEFT   GPIO_NUM_7
#define TB_PIN_RIGHT  GPIO_NUM_6
#define TB_PIN_BTN    GPIO_NUM_3

typedef enum {
    TB_EVENT_UP,
    TB_EVENT_DOWN,
    TB_EVENT_LEFT,
    TB_EVENT_RIGHT,
    TB_EVENT_BTN_CLICK
} trackball_event_t;

static QueueHandle_t trackball_evt_queue = NULL;

// ==========================================================
// 2. NimBLE 蓝牙服务 (UUID 0x1111 / 0x3333)
// ==========================================================
#define DEVICE_NAME "Cyberry_Wand"

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t notify_char_handle;
static uint8_t own_addr_type;

void ble_send_notify(const char *msg) {
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
        ble_gatts_notify_custom(conn_handle, notify_char_handle, om);
    }
}

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    return 0;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1111),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x3333),
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &notify_char_handle,
            }, { 0, }
        },
    }, { 0, }
};

static void ble_app_advertise(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE 已连接!");
            } else {
                ble_app_advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "BLE 断开，重新广播...");
            ble_app_advertise();
            break;
    }
    return 0;
}

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

static void ble_app_on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_app_advertise();
}

void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE 宿主任务启动");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    nimble_port_freertos_init(ble_host_task);
}

// ==========================================================
// 3. 轨迹球 GPIO 中断 (ISR)
// ==========================================================
static void IRAM_ATTR trackball_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    trackball_event_t evt;

    switch(gpio_num) {
        case TB_PIN_UP:    evt = TB_EVENT_UP; break;
        case TB_PIN_DOWN:  evt = TB_EVENT_DOWN; break;
        case TB_PIN_LEFT:  evt = TB_EVENT_LEFT; break;
        case TB_PIN_RIGHT: evt = TB_EVENT_RIGHT; break;
        case TB_PIN_BTN:   evt = TB_EVENT_BTN_CLICK; break;
        default: return;
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(trackball_evt_queue, &evt, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void trackball_init(void)
{
    trackball_evt_queue = xQueueCreate(10, sizeof(trackball_event_t));

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << TB_PIN_UP)    | (1ULL << TB_PIN_DOWN) |
                           (1ULL << TB_PIN_LEFT)  | (1ULL << TB_PIN_RIGHT) |
                           (1ULL << TB_PIN_BTN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);

    gpio_isr_handler_add(TB_PIN_UP,    trackball_isr_handler, (void*) TB_PIN_UP);
    gpio_isr_handler_add(TB_PIN_DOWN,  trackball_isr_handler, (void*) TB_PIN_DOWN);
    gpio_isr_handler_add(TB_PIN_LEFT,  trackball_isr_handler, (void*) TB_PIN_LEFT);
    gpio_isr_handler_add(TB_PIN_RIGHT, trackball_isr_handler, (void*) TB_PIN_RIGHT);
    gpio_isr_handler_add(TB_PIN_BTN,   trackball_isr_handler, (void*) TB_PIN_BTN);

    ESP_LOGI(TAG, "Trackball initialized.");
}

// ==========================================================
// 4. 数据处理主任务 (通过 BLE Notify 发送)
// ==========================================================
void trackball_task(void* arg)
{
    trackball_event_t evt;
    TickType_t last_btn_tick = 0;
    TickType_t last_dir_tick = 0;
    const TickType_t DIR_COOLDOWN = pdMS_TO_TICKS(200);

    while (1) {
        if (xQueueReceive(trackball_evt_queue, &evt, portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();

            if (evt == TB_EVENT_BTN_CLICK) {
                if ((now - last_btn_tick) > pdMS_TO_TICKS(50)) {
                    last_btn_tick = now;
                    ESP_LOGW(TAG, "BUTTON CLICKED -> SwipeClick");
                    ble_send_notify("SwipeClick");
                }
            }
            else {
                if ((now - last_dir_tick) > DIR_COOLDOWN) {
                    last_dir_tick = now;

                    switch (evt) {
                        case TB_EVENT_UP:
                            ESP_LOGI(TAG, "Moving UP -> SwipeUp");
                            ble_send_notify("SwipeUp");
                            break;
                        case TB_EVENT_DOWN:
                            ESP_LOGI(TAG, "Moving DOWN -> SwipeDown");
                            ble_send_notify("SwipeDown");
                            break;
                        case TB_EVENT_LEFT:
                            ESP_LOGI(TAG, "Moving LEFT -> SwipeLeft");
                            ble_send_notify("SwipeLeft");
                            break;
                        case TB_EVENT_RIGHT:
                            ESP_LOGI(TAG, "Moving RIGHT -> SwipeRight");
                            ble_send_notify("SwipeRight");
                            break;
                        default: break;
                    }
                }
            }
        }
    }
}

// ==========================================================
// 5. 主函数
// ==========================================================
void app_main(void)
{
    ble_init();
    trackball_init();
    xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);
}
