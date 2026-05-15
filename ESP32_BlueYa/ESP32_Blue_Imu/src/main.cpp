#include <stdio.h>
#include <string.h>
#include <cmath> 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "driver/i2c.h"

// TensorFlow Lite Micro
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model.h" 

static const char *TAG = "MAGIC_WAND";

// ==========================================================
// 1. 硬件与 AI 参数配置
// ==========================================================
#define I2C_MASTER_SDA_IO 12
#define I2C_MASTER_SCL_IO 13
#define I2C_MASTER_NUM I2C_NUM_0
#define MPU6050_ADDR 0x68 

#define WINDOW_SIZE 60        // 60 帧
#define PRE_TRIGGER 15        // 触发前保留 10 帧
#define MOTION_THRESHOLD 8000 // 触发阈值

static uint16_t notify_chr_val_handle;
static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// 环形缓冲区
float ring_buffer[WINDOW_SIZE][6];
int ring_ptr = 0;

// AI 引擎变量
constexpr int kTensorArenaSize = 100 * 1024;
uint8_t tensor_arena[kTensorArenaSize];
const tflite::Model* model = nullptr;  // <--- 就是漏了这一行致命声明！！！
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;

// ==========================================================
// 2. 硬件初始化 (I2C & MPU6050)
// ==========================================================
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 400000;
    i2c_param_config(I2C_MASTER_NUM, &conf);
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

void mpu6050_init_native() {
    uint8_t write_buf[2];
    write_buf[0] = 0x6B; write_buf[1] = 0x00;
    i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, write_buf, 2, 100);
    write_buf[0] = 0x1B; write_buf[1] = 0x08; // ±500°/s
    i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR, write_buf, 2, 100);
    ESP_LOGI(TAG, "✅ MPU6050 初始化完成");
}

// ==========================================================
// 3. 蓝牙 GATT 服务配置
// ==========================================================
static void start_advertising(void);

void server_send_data(const char* msg) {
    if (current_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
        ble_gatts_notify_custom(current_conn_handle, notify_chr_val_handle, om);
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
        .arg = NULL,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &notify_chr_val_handle,
    }, 
    { 0 } // 结尾标志
};

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .includes = NULL,
        .characteristics = gatt_svr_chrs
    }, 
    { 0 } // 结尾标志
};

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        current_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "蓝牙已连接！");
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
    }
    return 0;
}

static void start_advertising(void) {
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"Cyberry_Wand";
    fields.name_len = 12;
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv, ble_gap_event, NULL);
}

// ==========================================================
// 4. AI 引擎初始化
// ==========================================================
void ai_init() {
    model = tflite::GetModel(magic_wand_model_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "❌ 模型版本不匹配!");
        return;
    }

    static tflite::MicroMutableOpResolver<10> resolver;
    resolver.AddConv2D(); resolver.AddMaxPool2D(); resolver.AddFullyConnected();
    resolver.AddReshape(); resolver.AddSoftmax(); resolver.AddRelu();
    resolver.AddExpandDims(); resolver.AddShape(); resolver.AddStridedSlice(); resolver.AddPack();

    static tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;
    
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "❌ 内存分配失败 (请检查 RAM 剩余空间)");
        return;
    }
    
    input = interpreter->input(0);
    output = interpreter->output(0);
    ESP_LOGI(TAG, "🧠 AI 引擎加载完毕! 准备施法...");
}

// ==========================================================
// 5. 核心识别任务
// ==========================================================
void magic_wand_task(void *pvParameters) {
    mpu6050_init_native();
    uint8_t data[14];
    int post_count = 0;
    bool triggered = false;

    while (1) {
        if (i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR, (uint8_t[]){0x3B}, 1, data, 14, 10) == ESP_OK) {
            float ax = (int16_t)((data[0] << 8) | data[1]);
            float ay = (int16_t)((data[2] << 8) | data[3]);
            float az = (int16_t)((data[4] << 8) | data[5]);
            float gx = (int16_t)((data[8] << 8) | data[9]) + 478.0f;
            float gy = (int16_t)((data[10] << 8) | data[11]) + 100.0f;
            float gz = (int16_t)((data[12] << 8) | data[13]) + 20.0f;

            if (input == nullptr) {
                ESP_LOGE(TAG, "AI 尚未就绪...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            // 写入环形缓冲区
            ring_buffer[ring_ptr][0] = ax; ring_buffer[ring_ptr][1] = ay; ring_buffer[ring_ptr][2] = az;
            ring_buffer[ring_ptr][3] = gx; ring_buffer[ring_ptr][4] = gy; ring_buffer[ring_ptr][5] = gz;
            ring_ptr = (ring_ptr + 1) % WINDOW_SIZE;

            if (!triggered) {
                if (std::abs(gx) > MOTION_THRESHOLD || std::abs(gy) > MOTION_THRESHOLD || std::abs(gz) > MOTION_THRESHOLD) {
                    triggered = true;
                    post_count = 0;
                    ESP_LOGW(TAG, "💥 检测到挥动！开始捕捉轨迹...");
                }
            } else {
                post_count++;
                if (post_count >= (WINDOW_SIZE - PRE_TRIGGER)) {
                    for (int i = 0; i < WINDOW_SIZE; i++) {
                        int idx = (ring_ptr + i) % WINDOW_SIZE;
                        for (int j = 0; j < 6; j++) {
                            input->data.f[i * 6 + j] = ring_buffer[idx][j] / 32768.0f;
                        }
                    }

                    int64_t start_time = esp_timer_get_time();
                    if (interpreter->Invoke() == kTfLiteOk) {
                        int64_t end_time = esp_timer_get_time();
                        
                        float p_up    = output->data.f[0]; 
                        float p_down  = output->data.f[1]; 
                        float p_left  = output->data.f[2]; 
                        float p_right = output->data.f[3]; 
                        float p_none  = output->data.f[4]; 
                        
                        ESP_LOGI(TAG, "推理耗时: %lld us", (end_time - start_time));
                        ESP_LOGI(TAG, "上:%.0f%% 下:%.0f%% 左:%.0f%% 右:%.0f%% 无:%.0f%%", 
                                 p_up*100, p_down*100, p_left*100, p_right*100, p_none*100);

                        if (p_up > 0.8f) {
                            ESP_LOGE(TAG, "✨✨ 施法: 上滑 (Swipe Up) !");
                            server_send_data("Action: SwipeUp");
                        } else if (p_down > 0.8f) {
                            ESP_LOGE(TAG, "✨✨ 施法: 下滑 (Swipe Down) !");
                            server_send_data("Action: SwipeDown");
                        } else if (p_left > 0.8f) {
                            ESP_LOGE(TAG, "✨✨ 施法: 左挥 (Swipe Left) !");
                            server_send_data("Action: SwipeLeft");
                        } else if (p_right > 0.8f) {
                            ESP_LOGE(TAG, "✨✨ 施法: 右挥 (Swipe Right) !");
                            server_send_data("Action: SwipeRight");
                        } else {
                            ESP_LOGI(TAG, "🤔 没看懂这是什么咒语...");
                        }
                    }
                    triggered = false;
                    ESP_LOGI(TAG, "🛑 冷却中...");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "🟢 准备就绪，请施法！");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// ==========================================================
// 6. 主函数入口
// ==========================================================
extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(4000)); 
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ai_init();
    ESP_ERROR_CHECK(i2c_master_init());

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);

    ble_svc_gap_device_name_set("Cyberry_Wand");
    ble_hs_cfg.sync_cb = [](){ start_advertising(); };
    
    nimble_port_freertos_init([](void*p){ nimble_port_run(); });

    xTaskCreate(magic_wand_task, "wand", 8192, NULL, 5, NULL);
}