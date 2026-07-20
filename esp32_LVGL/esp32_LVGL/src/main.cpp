#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tft_display.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h" // 引入 WebSocket 客户端
#include "nvs_flash.h"
// 引入 LVGL 核心与移植包
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "math.h" // 引入数学库，后面会用到正弦函数来生成测试音频数据
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"


// C 头文件需要 extern "C" 包装
extern "C" {
#include "MPU6050.h"
#include "bmp280.h"
#include "my_wifi.h"
#include "app_mqtt.h"
#include "my_ble.h"
#include "my_uart.h"
#include "ui_ar_glass.h"
#include "ui_menu_screen.h"
#include "ui_globals.h"
#include "ui_novel_screen.h"
#include "ui_manager.h"
#include "light_sensor.h"
#include "gps.h"
#include "max30102.h"
#include "paj7620.h"
#include "encoder.h"
#include "motor_pwm.h"
}

// ==========================================
// 🧙 TensorFlow Lite Micro 头文件
// ==========================================
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "model.h"


// ==========================================
// ? 服务器配置 (请改成你运行 Python 脚本的电脑 IP)
// ==========================================
const char* websocket_url = "ws://124.220.224.189:8765/"; 
esp_websocket_client_handle_t ws_client;
LV_FONT_DECLARE(my_font_cn_16);



// 统一的 I2C 引脚和参数配置（根据你 MPU6050 里的设置提取出来）
#define I2C_MASTER_SCL_IO           1
#define I2C_MASTER_SDA_IO           2
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000

static const char *TAG = "MAIN";

// I2C 总线互斥锁（保护 MPU6050/BMP280/MAX30102 共享总线）
SemaphoreHandle_t i2c_mutex = NULL;

// ==========================================================
// 🧙 AI 引擎全局变量与配置
// ==========================================================
#define WINDOW_SIZE 60        // 60 帧
#define PRE_TRIGGER 15        // 触发前保留 15 帧
#define MOTION_THRESHOLD 8000 // 触发阈值

float ring_buffer[WINDOW_SIZE][6];
int ring_ptr = 0;

constexpr int kTensorArenaSize = 100 * 1024;
uint8_t *tensor_arena = NULL;
const tflite::Model* magic_model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* model_input = nullptr;
TfLiteTensor* model_output = nullptr;

void ai_init() {
    // 1. 在 PSRAM 中分配 100KB 的张量运算内存
    tensor_arena = (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
    if (tensor_arena == NULL) {
        ESP_LOGE(TAG, "❌ PSRAM 内存分配失败!");
        return;
    }

    magic_model = tflite::GetModel(magic_wand_model_tflite);
    if (magic_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "❌ 模型版本不匹配!");
        return;
    }

    static tflite::MicroMutableOpResolver<10> resolver;
    resolver.AddConv2D(); resolver.AddMaxPool2D(); resolver.AddFullyConnected();
    resolver.AddReshape(); resolver.AddSoftmax(); resolver.AddRelu();
    resolver.AddExpandDims(); resolver.AddShape(); resolver.AddStridedSlice(); resolver.AddPack();

    static tflite::MicroInterpreter static_interpreter(magic_model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "❌ AI 内存分配失败");
        return;
    }

    model_input = interpreter->input(0);
    model_output = interpreter->output(0);
    ESP_LOGI(TAG, "🧙 AI 引擎加载完毕! 魔杖就绪...");
}
// ----------------------------------------------------
// 全局唯一的 I2C 总线初始化函数
// ----------------------------------------------------
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = (gpio_num_t)I2C_MASTER_SDA_IO;
    conf.scl_io_num = (gpio_num_t)I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err == ESP_OK) {
        i2c_mutex = xSemaphoreCreateMutex();
    }
    return err;
}


// =========================================================
// ? 设置断网情况下的默认开机时间
// =========================================================
void set_default_time(void) {
    // 1. 先设置好时区，保证我们设定的 12:00 是北京时间的 12:00
    setenv("TZ", "CST-8", 1);
    tzset();

    // 2. 构造 2026年1月1日 12:00:00 的时间结构体
    struct tm tm_default;
    memset(&tm_default, 0, sizeof(tm_default));
    tm_default.tm_year = 2026 - 1900; // C语言标准：年份从 1900 算起
    tm_default.tm_mon  = 1 - 1;       // C语言标准：月份是 0 到 11
    tm_default.tm_mday = 1;           // 1日
    tm_default.tm_hour = 12;          // 12点
    tm_default.tm_min  = 0;           // 0分
    tm_default.tm_sec  = 0;           // 0秒

    // 3. 将结构体转换为时间戳
    time_t t = mktime(&tm_default);

    // 4. 强行写入 ESP32 的底层系统时钟
    struct timeval now;
    now.tv_sec = t;
    now.tv_usec = 0;
    settimeofday(&now, NULL);
    
    ESP_LOGI("TIME", "? 无网默认开机时间已设置为 2026-01-01 12:00:00");
}
// =========================================================
// ?? 初始化网络时间同步
// =========================================================
void time_sync_init(void) {
    ESP_LOGI("TIME", "正在初始化 SNTP 时间同步...");
    
    // 设置时区为中国标准时间 (UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();

    // 配置 NTP 服务器
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");    // 国际公共 NTP
    esp_sntp_setservername(1, "ntp.aliyun.com");  // 阿里云 NTP (国内备用，速度快)
    esp_sntp_init();
}




// ==========================================
// ? WebSocket 事件回调：接收音频并播放
// ==========================================
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, " 已连接到基站服务器!");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, " 与基站断开连接，尝试重连...");
            break;
        case WEBSOCKET_EVENT_DATA:
            // op_code == 2 表示收到的是二进制流 (BIN)，即 Python 发来的 PCM 音频数据
            if (data->op_code == 2 && data->data_len > 0) {
                // 音频驱动已移除，GPIO 5/6/7 已释放
            }
            break;
    }
}

// 传感器数据定时上传 MQTT（每 5 秒）
static void sensor_mqtt_task(void *arg) {
    extern void app_mqtt_publish(const char *topic, const char *data);
    extern esp_err_t bmp280_read_temp(i2c_port_t i2c_num, float *temperature);
    extern gps_data_t gps_get_data(void);
    extern int32_t encoder_get_count(void);
    char buf[32];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        // 光照
        float lux = light_sensor_get_lux();
        snprintf(buf, sizeof(buf), "%.1f", lux);
        app_mqtt_publish("esp32/glass/light", buf);
        ESP_LOGI("SENSOR", "光照: %.1f lux", lux);

        // 温度
        float temp;
        if (bmp280_read_temp(I2C_NUM_0, &temp) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%.1f", temp);
            app_mqtt_publish("esp32/glass/temp", buf);
            ESP_LOGI("SENSOR", "温度: %.1f C", temp);
        }

        // 编码器
        int32_t enc_count = encoder_get_count();
        ESP_LOGI("ENCODER", "计数: %d", enc_count);

        // 步数
        extern uint32_t step_count;
        ESP_LOGI("STEP", "当前步数: %lu", step_count);

        // GPS 经纬度
        gps_data_t gps = gps_get_data();
        if (gps.valid) {
            snprintf(buf, sizeof(buf), "%.6f", gps.latitude);
            app_mqtt_publish("esp32/glass/lat", buf);
            snprintf(buf, sizeof(buf), "%.6f", gps.longitude);
            app_mqtt_publish("esp32/glass/lng", buf);
            ESP_LOGI("GPS", "时间: %s | 卫星: %d", gps.utc_time, gps.satellites);
            ESP_LOGI("GPS", "纬度: %.6f | 经度: %.6f", gps.latitude, gps.longitude);
            ESP_LOGI("GPS", "海拔: %.1fm | 速度: %.1fkm/h", gps.altitude, gps.speed_kmh);
        } else {
            ESP_LOGI("GPS", "等待定位...");
        }
    }
}

// 编码器扫描任务（每200ms读取，大于2上滑，小于-2下滑）
static void encoder_scan_task(void *arg) {
    extern int32_t encoder_get_count(void);
    extern void encoder_reset(void);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        int32_t count = encoder_get_count();
        encoder_reset();

        // 大于2判定为上滑，小于-2判定为下滑
        if (count > 2) {
            ESP_LOGI("ENCODER", "上滑 (count=%d)", count);
            motor_pulse_short(); // 编码器旋转震动反馈
            if (ui_cmd_queue != NULL) {
                ui_cmd_t cmd = UI_CMD_UP;
                xQueueSend(ui_cmd_queue, &cmd, 0);
            }
        } else if (count < -2) {
            ESP_LOGI("ENCODER", "下滑 (count=%d)", count);
            motor_pulse_short(); // 编码器旋转震动反馈
            if (ui_cmd_queue != NULL) {
                ui_cmd_t cmd = UI_CMD_DOWN;
                xQueueSend(ui_cmd_queue, &cmd, 0);
            }
        }
    }
}

// 按键扫描任务（映射到 UI 手势指令）
static void btn_scan_task(void *arg) {
    const int btn_pins[] = {42, 41, 15, 16, 21};
    const ui_cmd_t btn_cmds[] = {UI_CMD_UP, UI_CMD_DOWN, UI_CMD_LEFT, UI_CMD_RIGHT, UI_CMD_CIRCLE};
    const char *btn_names[] = {"上", "下", "左", "右", "画圈"};
    int last_state[5] = {1, 1, 1, 1, 1};

    while (1) {
        for (int i = 0; i < 5; i++) {
            int state = gpio_get_level((gpio_num_t)btn_pins[i]);
            if (state == 0 && last_state[i] == 1) {
                ESP_LOGI("BTN", "按键: %s (GPIO %d)", btn_names[i], btn_pins[i]);
                motor_pulse_short(); // 按键震动反馈
                if (ui_cmd_queue != NULL) {
                    xQueueSend(ui_cmd_queue, &btn_cmds[i], 0);
                }
            }
            last_state[i] = state;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ==========================================================
// 🧙 核心魔杖识别任务 (使用 TFLite Micro)
// ==========================================================
void magic_wand_task(void *pvParameters) {
    uint8_t data[14];
    int post_count = 0;
    bool triggered = false;

    while (1) {
        bool i2c_read_success = false;

        // 使用 I2C 互斥锁，防止与其他 I2C 传感器冲突
        if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            // 读取 MPU6050 (0x68) 的加速度与陀螺仪数据
            if (i2c_master_write_read_device(I2C_MASTER_NUM, 0x68, (uint8_t[]){0x3B}, 1, data, 14, 10) == ESP_OK) {
                i2c_read_success = true;
            } else {
                ESP_LOGE("WAND", "I2C 读取 MPU6050 失败！");
            }
            xSemaphoreGive(i2c_mutex);
        }

        if (i2c_read_success) {
            float ax = (int16_t)((data[0] << 8) | data[1]);
            float ay = (int16_t)((data[2] << 8) | data[3]);
            float az = (int16_t)((data[4] << 8) | data[5]);
            float gx = (int16_t)((data[8] << 8) | data[9]) + 478.0f;
            float gy = (int16_t)((data[10] << 8) | data[11]) + 100.0f;
            float gz = (int16_t)((data[12] << 8) | data[13]) + 20.0f;

            // 发送加速度到计步器队列
            if (accel_queue != NULL) {
                float raw_norm_acc = sqrtf(ax*ax + ay*ay + az*az);
                xQueueSend(accel_queue, &raw_norm_acc, 0);
                // 调试：打印原始加速度
                static int acc_debug = 0;
                if (++acc_debug >= 50) {
                    ESP_LOGI("WAND", "加速度: %.1f (ax:%.0f ay:%.0f az:%.0f)", raw_norm_acc, ax, ay, az);
                    acc_debug = 0;
                }
            }

            if (model_input == nullptr) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            // 数据写入环形缓冲区
            ring_buffer[ring_ptr][0] = ax; ring_buffer[ring_ptr][1] = ay; ring_buffer[ring_ptr][2] = az;
            ring_buffer[ring_ptr][3] = gx; ring_buffer[ring_ptr][4] = gy; ring_buffer[ring_ptr][5] = gz;
            ring_ptr = (ring_ptr + 1) % WINDOW_SIZE;

            if (!triggered) {
                if (fabs(gx) > MOTION_THRESHOLD || fabs(gy) > MOTION_THRESHOLD || fabs(gz) > MOTION_THRESHOLD) {
                    triggered = true;
                    post_count = 0;
                    ESP_LOGW(TAG, "💥 捕捉到魔杖挥动！");
                }
            } else {
                post_count++;
                if (post_count >= (WINDOW_SIZE - PRE_TRIGGER)) {
                    // 填装 AI 输入张量
                    for (int i = 0; i < WINDOW_SIZE; i++) {
                        int idx = (ring_ptr + i) % WINDOW_SIZE;
                        for (int j = 0; j < 6; j++) {
                            model_input->data.f[i * 6 + j] = ring_buffer[idx][j] / 32768.0f;
                        }
                    }

                    // 运行模型推理
                    if (interpreter->Invoke() == kTfLiteOk) {
                        // 7个动作: 0=下 1=上 2=左 3=右 4=左敲 5=右敲 6=无动作
                        float p_down    = model_output->data.f[0];
                        float p_up      = model_output->data.f[1];
                        float p_left    = model_output->data.f[2];
                        float p_right   = model_output->data.f[3];
                        float p_tap_l   = model_output->data.f[4];
                        float p_tap_r   = model_output->data.f[5];

                        ui_cmd_t cmd = UI_CMD_NONE;

                        // 映射魔杖动作到统一的 UI 指令
                        if (p_up > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 上滑 (%.0f%%)", p_up*100); cmd = UI_CMD_UP;
                        } else if (p_down > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 下滑 (%.0f%%)", p_down*100); cmd = UI_CMD_DOWN;
                        } else if (p_left > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 左挥 (%.0f%%)", p_left*100); cmd = UI_CMD_LEFT;
                        } else if (p_right > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 右挥 (%.0f%%)", p_right*100); cmd = UI_CMD_RIGHT;
                        } else if (p_tap_l > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 左敲 (%.0f%%)", p_tap_l*100); cmd = UI_CMD_CIRCLE;
                        } else if (p_tap_r > 0.8f) {
                            ESP_LOGW(TAG, "✨ 魔杖施法: 右敲 (%.0f%%)", p_tap_r*100); cmd = UI_CMD_CIRCLE;
                        }

                        // 如果识别成功，触发震动反馈并发送给 UI 队列
                        if (cmd != UI_CMD_NONE) {
                            motor_pulse_short();
                            if (ui_cmd_queue != NULL) {
                                xQueueSend(ui_cmd_queue, &cmd, 0);
                            }
                        }
                    }
                    triggered = false;
                    vTaskDelay(pdMS_TO_TICKS(100)); // 冷却期
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 保证高频采样（约 100Hz）
    }
}

// UART0 接收任务（GPIO 13 读取外部芯片数据）
static void uart0_rx_task(void *arg) {
    uint8_t buf[128];
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, buf, sizeof(buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            buf[len] = '\0';
            ESP_LOGI("UART0_RX", "收到 %d 字节: %s", len, (char*)buf);
        }
    }
}

extern "C" void app_main(void) {
    // ? 1. 必须先初始化 NVS，否则 Wi-Fi 必崩溃！
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 按键引脚初始化（输入 + 内部上拉，按下为低电平）
    int btn_pins[] = {42, 41, 15, 16, 21};
    for (int i = 0; i < 5; i++) {
        gpio_reset_pin((gpio_num_t)btn_pins[i]);
        gpio_set_direction((gpio_num_t)btn_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)btn_pins[i], GPIO_PULLUP_ONLY);
    }
    xTaskCreatePinnedToCore(btn_scan_task, "btn_scan", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(sensor_mqtt_task, "sensor_mqtt", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(encoder_scan_task, "enc_scan", 4096, NULL, 3, NULL, 0);

    my_ble_init("My_Smart_JARVIS");
    ESP_LOGI(TAG, "1. 启动物理屏幕驱动...");
    lcd_init();
    
    ESP_LOGI(TAG, "2. 初始化 LVGL 移植层...");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG, "3. 将屏幕挂载到 LVGL...");
    // 先设置镜像，再注册显示驱动
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));

    // 检查 PSRAM 是否可用
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM 总大小: %d KB", psram_size / 1024);

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io_handle;
    disp_cfg.panel_handle = panel_handle;
    disp_cfg.buffer_size = 240 * 240 / 5;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = 240;
    disp_cfg.vres = 240;
    disp_cfg.monochrome = false;
    disp_cfg.flags.buff_dma = false;
    lvgl_port_add_disp(&disp_cfg);
    set_default_time(); // 设置默认时间，防止无网时显示 1970 年

    ESP_LOGI(TAG, "3.5 初始化 LVGL 扩展库 (SJPG/PNG/BMP 解码器)...");
    lv_extra_init();

    // 启动你刚刚写好的串口模块
    my_uart_init();

    // UART0 RX 重映射到 GPIO 13（日志走 USB Serial/JTAG，不占引脚）
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, 13, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "UART0 RX 已映射到 GPIO 13");

    // 启动 UART0 接收任务（栈放 PSRAM）
    StackType_t *uart0_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StaticTask_t *uart0_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (uart0_stack && uart0_tcb) {
        xTaskCreateStaticPinnedToCore(uart0_rx_task, "uart0_rx", 4096/sizeof(StackType_t), NULL, 3, uart0_stack, uart0_tcb, 0);
    }

    // ==========================================
    // ? 核心大换血：启动 UI 大管家
    // ==========================================
    ESP_LOGI(TAG, "启动 UI 大管家...");
    // ?? 注意：不要在这里加 lvgl_port_lock() 了！
    // 因为 ui_manager_init 内部已经自己加锁，并初始化了所有屏幕！
    ui_manager_init();
    
    // 5. 初始化硬件 I2C 总线
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C 硬件总线初始化完毕！");

    // 6. 连接 Wi-Fi (依赖前面的 NVS 初始化)
    wifi_init_sta();

    // 7. 初始化传感器
    if (mpu6050_init_all() == ESP_OK) {
        ESP_LOGI(TAG, "MPU6050 唤醒成功！");
    }
    if (bmp280_init(I2C_MASTER_NUM) == ESP_OK) {
        ESP_LOGI(TAG, "BMP280 配置成功！");
    }

    // 8. 初始化光照传感器（TEMT6000，接在 GPIO 4）
    if (light_sensor_init() == ESP_OK) {
        ESP_LOGI(TAG, "光照传感器初始化完成！");
    }

    // 8.5 初始化旋转编码器（GPIO 47=A相, GPIO 48=B相）
    if (encoder_init() == ESP_OK) {
        ESP_LOGI(TAG, "旋转编码器初始化完成！");
    }

    // 8.6 初始化震动马达（GPIO 14）
    motor_pwm_init();

    // 9. 初始化 GPS 模块（ATGM336H，UART1，GPIO 17/18）
    if (gps_init() == ESP_OK) {
        ESP_LOGI(TAG, "GPS 模块初始化完成！");
    }

    // 10. 初始化 MAX30102 心率血氧传感器（GPIO 1/2，I2C_NUM_1）
    if (max30102_init() == ESP_OK) {
        ESP_LOGI(TAG, "MAX30102 初始化完成！");
    }

    // 11. 初始化 PAJ7620 手势识别传感器（GPIO 38/39，I2C_NUM_1）
    if (paj7620_init() == ESP_OK) {
        paj7620_start_task();
        ESP_LOGI(TAG, "PAJ7620 手势传感器初始化完成！");
    }



    // 9. 配置并启动 WebSocket 客户端
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = websocket_url;
    websocket_cfg.reconnect_timeout_ms = 5000;
    ws_client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
    // MQTT 和 WebSocket 在 WiFi 连上后才启动（my_wifi.c 的 GOT_IP 回调）
    time_sync_init(); // 启动时间同步，确保时间显示正确

    // ==========================================
    // 🧙 【新增】初始化 AI 并启动魔杖动作识别任务
    // ==========================================
    ai_init();

    // TFLite 运算栈较深，使用 8192 字节，并分配在 PSRAM 中
    StackType_t *wand_stack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    StaticTask_t *wand_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (wand_stack && wand_tcb) {
        xTaskCreateStatic(magic_wand_task, "ai_wand", 8192/sizeof(StackType_t), NULL, 5, wand_stack, wand_tcb);
    }

    // ==========================================
    // 🚶 【新增】创建计步器队列和任务
    // ==========================================
    accel_queue = xQueueCreate(20, sizeof(float));
    if (accel_queue != NULL) {
        xTaskCreatePinnedToCore(step_counter_task, "step_counter", 4096, NULL, 4, NULL, 0);
        ESP_LOGI(TAG, "计步器任务已启动");
    }

    // 继续启动剩余的传感器任务（BMP280 等）
    StackType_t *bmp_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StaticTask_t *bmp_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (bmp_stack && bmp_tcb) xTaskCreateStatic(read_bmp280_task, "bmp280", 4096/sizeof(StackType_t), NULL, 4, bmp_stack, bmp_tcb);

    // 时间刷新任务
    xTaskCreate(ui_time_update_task, "ui_time_task", 1024 * 2, NULL, 2, NULL);
    // 11. 主循环挂起
    while (1) {
        app_mqtt_publish("home/status/sensor", "TEMP:25C");
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
