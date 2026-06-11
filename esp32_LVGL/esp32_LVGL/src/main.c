#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tft_display.h"
#include "driver/i2c.h"
#include "esp_websocket_client.h" // 引入 WebSocket 客户端
#include "nvs_flash.h"
// 引入 LVGL 核心与移植包
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "math.h" // 引入数学库，后面会用到正弦函数来生成测试音频数据
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"


#include "MPU6050.h"
#include "bmp280.h"
#include "my_wifi.h"
#include "audio_driver.h"
#include "app_mqtt.h"
#include "my_ble.h" // 引入我们刚才写的蓝牙模块头文件
#include "my_uart.h" // ? 新增：串口通信模块

#include "ui_ar_glass.h"
#include "ui_menu_screen.h"
#include "ui_globals.h" // 引入全局变量枢纽
#include "ui_novel_screen.h" // 引入小说屏幕的头文件，里面有初始化函数声明
#include "ui_manager.h"
#include "light_sensor.h"  // 光照传感器驱动（TEMT6000，GPIO 4）
#include "gps.h"           // GPS 模块驱动（ATGM336H，UART1）
#include "max30102.h"      // MAX30102 心率血氧传感器


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
// ----------------------------------------------------
// 全局唯一的 I2C 总线初始化函数
// ----------------------------------------------------
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}


// =========================================================
// ? 设置断网情况下的默认开机时间
// =========================================================
void set_default_time(void) {
    // 1. 先设置好时区，保证我们设定的 12:00 是北京时间的 12:00
    setenv("TZ", "CST-8", 1);
    tzset();

    // 2. 构造 2026年1月1日 12:00:00 的时间结构体
    struct tm tm_default = {0};
    tm_default.tm_year = 2026 - 1900; // C语言标准：年份从 1900 算起
    tm_default.tm_mon  = 1 - 1;       // C语言标准：月份是 0 到 11
    tm_default.tm_mday = 1;           // 1日
    tm_default.tm_hour = 12;          // 12点
    tm_default.tm_min  = 0;           // 0分
    tm_default.tm_sec  = 0;           // 0秒

    // 3. 将结构体转换为时间戳
    time_t t = mktime(&tm_default);

    // 4. 强行写入 ESP32 的底层系统时钟
    struct timeval now = { .tv_sec = t, .tv_usec = 0 };
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
                // 核心魔法：将收到的网络音频块，直接塞给 I2S 驱动缓冲区！
                // I2S 驱动内部配置了 portMAX_DELAY，如果底层播放没播完，这里会自动阻塞，完美控制网速不溢出
                audio_driver_play(data->data_ptr, data->data_len);
            }
            break;
    }
}

void app_main(void) {
    // ? 1. 必须先初始化 NVS，否则 Wi-Fi 必崩溃！
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    my_ble_init("My_Smart_JARVIS");
    ESP_LOGI(TAG, "1. 启动物理屏幕驱动...");
    lcd_init();
    
    ESP_LOGI(TAG, "2. 初始化 LVGL 移植层...");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    ESP_LOGI(TAG, "3. 将屏幕挂载到 LVGL...");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = 240 * 240 / 10, 
        .double_buffer = true,
        .hres = 240,
        .vres = 240,
        .monochrome = false,
        .flags = { .buff_dma = true }
    };
    lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    set_default_time(); // 设置默认时间，防止无网时显示 1970 年

    ESP_LOGI(TAG, "3.5 初始化 LVGL 扩展库 (SJPG/PNG/BMP 解码器)...");
    lv_extra_init();

    // 启动你刚刚写好的串口模块
    my_uart_init();

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

    // 9. 初始化 GPS 模块（ATGM336H，UART1，GPIO 17/18）
    if (gps_init() == ESP_OK) {
        ESP_LOGI(TAG, "GPS 模块初始化完成！");
    }

    // 10. 初始化 MAX30102 心率血氧传感器（GPIO 1/2，I2C_NUM_1）
    if (max30102_init() == ESP_OK) {
        ESP_LOGI(TAG, "MAX30102 初始化完成！");
    }

    // 8. 初始化音频驱动
    if (audio_driver_init() != ESP_OK) {
        printf("? 音频模块初始化失败！请检查日志。\n");
        return;
    }
    
    // 9. 配置并启动 WebSocket 客户端
    esp_websocket_client_config_t websocket_cfg = {
        .uri = websocket_url,
        .reconnect_timeout_ms = 5000, 
    };
    ws_client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
    esp_websocket_client_start(ws_client);
    app_mqtt_start();
    time_sync_init(); // 启动时间同步，确保时间显示正确
    // 10. 创建传感器读取任务
    // ?? 注意：前提是你已经在其他文件实现了 read_mpu6050_task，否则编译会报错找不到该函数
    xTaskCreate(read_mpu6050_task, "read_mpu6050_task", 4096, NULL, 5, NULL);
    xTaskCreate(read_bmp280_task, "read_bmp280_task", 4096, NULL, 4, NULL);
    // 创建时间刷新任务 (分配 2KB 栈空间，优先级设低一点比如 2)
    xTaskCreate(ui_time_update_task, "ui_time_task", 1024 * 2, NULL, 2, NULL);
    // 11. 主循环挂起
    while (1) {
        app_mqtt_publish("home/status/sensor", "TEMP:25C");
        vTaskDelay(pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
