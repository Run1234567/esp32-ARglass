#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
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
// MPU6050 I2C 配置
// ==========================================================
#define I2C_MASTER_SCL_IO        8
#define I2C_MASTER_SDA_IO        9
#define I2C_MASTER_NUM           I2C_NUM_0
#define I2C_MASTER_FREQ_HZ       400000
#define MPU6050_ADDR             0x68
#define MPU6050_WHO_AM_I_REG     0x75
#define MPU6050_PWR_MGMT_1_REG   0x6B
#define MPU6050_ACCEL_XOUT_H     0x3B

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t mpu_handle;

// ==========================================================
// 1. 轨迹球硬件引脚定义
// ==========================================================
#define TB_PIN_UP     GPIO_NUM_5
#define TB_PIN_DOWN   GPIO_NUM_4
#define TB_PIN_LEFT   GPIO_NUM_3
#define TB_PIN_RIGHT  GPIO_NUM_2
#define TB_PIN_BTN    GPIO_NUM_1

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
// 5. MPU6050 I2C 初始化与读取
// ==========================================================
void mpu6050_init(void)
{
    // 配置 I2C 主机
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "I2C 总线初始化成功 (SCL=%d, SDA=%d)", I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO);

    // 配置 MPU6050 设备
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6050_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &mpu_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "添加 MPU6050 设备失败: %s", esp_err_to_name(ret));
        return;
    }

    // 检测 MPU6050 是否存在 (读 WHO_AM_I)
    uint8_t who_am_i = 0;
    uint8_t reg = MPU6050_WHO_AM_I_REG;
    ret = i2c_master_transmit_receive(mpu_handle, &reg, 1, &who_am_i, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 MPU6050 WHO_AM_I 失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "MPU6050 WHO_AM_I = 0x%02X (应为 0x68)", who_am_i);

    // 唤醒 MPU6050 (清零 sleep bit)
    uint8_t wake_cmd[] = {MPU6050_PWR_MGMT_1_REG, 0x00};
    ret = i2c_master_transmit(mpu_handle, wake_cmd, 2, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "唤醒 MPU6050 失败: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "MPU6050 初始化完成");
}

void mpu6050_read_task(void *arg)
{
    uint8_t data[14];
    int16_t ax, ay, az, gx, gy, gz;

    while (1) {
        // 从 0x3B 开始读取 14 字节 (加速度 + 温度 + 陀螺仪)
        uint8_t reg = MPU6050_ACCEL_XOUT_H;
        esp_err_t ret = i2c_master_transmit_receive(mpu_handle, &reg, 1, data, 14, 1000);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MPU6050 读取失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 解析加速度 (原始值)
        ax = (int16_t)((data[0] << 8) | data[1]);
        ay = (int16_t)((data[2] << 8) | data[3]);
        az = (int16_t)((data[4] << 8) | data[5]);

        // 解析温度
        int16_t temp_raw = (int16_t)((data[6] << 8) | data[7]);
        float temp = temp_raw / 340.0f + 36.53f;

        // 解析陀螺仪 (原始值)
        gx = (int16_t)((data[8] << 8) | data[9]);
        gy = (int16_t)((data[10] << 8) | data[11]);
        gz = (int16_t)((data[12] << 8) | data[13]);

        // 打印到串口
        printf("Accel: X=%6d Y=%6d Z=%6d | Gyro: X=%6d Y=%6d Z=%6d | Temp: %.1f°C\n",
               ax, ay, az, gx, gy, gz, temp);

        vTaskDelay(pdMS_TO_TICKS(100));  // 10Hz 采样
    }
}

// ==========================================================
// 6. 主函数
// ==========================================================
void app_main(void)
{
    // I2C + MPU6050 初始化
    mpu6050_init();

    // BLE 初始化
    ble_init();

    // 轨迹球初始化
    trackball_init();
    xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);

    // MPU6050 读取任务
    xTaskCreate(mpu6050_read_task, "mpu6050_task", 4096, NULL, 4, NULL);
}
