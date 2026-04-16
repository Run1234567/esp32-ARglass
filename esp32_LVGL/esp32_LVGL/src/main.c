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

#include "MPU6050.h"
#include "MAX30105.h"
#include "my_wifi.h"
#include "audio_driver.h"
#include "app_mqtt.h"


#include "ui_ar_glass.h"
#include "ui_menu_screen.h"
#include "ui_globals.h" // 引入全局变量枢纽
#include "ui_novel_screen.h" // 引入小说屏幕的头文件，里面有初始化函数声明



// ==========================================
// 🌐 服务器配置 (请改成你运行 Python 脚本的电脑 IP)
// ==========================================
const char* websocket_url = "ws://124.220.224.189:8765/"; 
esp_websocket_client_handle_t ws_client;
LV_FONT_DECLARE(my_font_cn_16);

#define SAMPLE_RATE 16000       // 采样率必须和 audio_driver.c 里配置的一致
#define FREQUENCY 440.0         // 测试音频频率 440Hz (标准音A)
#define AMPLITUDE 8000          // 音量大小 (16位PCM最大是32767，8000是一个适中且不刺耳的音量)
#define BUFFER_SAMPLES 512      // 每次计算/发送的采样点数

// 定义一个双声道音频缓冲区 (每个采样点16位，左声道+右声道，所以数组大小要乘以2)
int16_t audio_buffer[BUFFER_SAMPLES * 2];

// 统一的 I2C 引脚和参数配置（根据你 MPU6050 里的设置提取出来）
#define I2C_MASTER_SCL_IO           1
#define I2C_MASTER_SDA_IO           2
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000

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



void read_max30105_task(void *pvParameters) {
    uint32_t red_val, ir_val;
    
    while (1) {
        if (max30105_read_fifo(I2C_MASTER_NUM, &red_val, &ir_val) == ESP_OK) {
            // 使用 printf 输出纯数据，格式为 "红光,红外光"
            // 这种格式可以直接被 Arduino IDE 或其他串口绘图仪识别并画出两条折线
            printf("%lu,%lu\n", red_val, ir_val);
        }
        
        // 绝对延时 5ms (相当于 200Hz 的读取频率)
        vTaskDelay(pdMS_TO_TICKS(5)); 
    }
}

// ==========================================
// 📡 WebSocket 事件回调：接收音频并播放
// ==========================================
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ 已连接到基站服务器!");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ 与基站断开连接，尝试重连...");
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
    // 💡 1. 必须先初始化 NVS，否则 Wi-Fi 必崩溃！
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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
    ESP_LOGI(TAG, "4. 绘制华丽的 UI...");
    if (lvgl_port_lock(0)) {
        
        ui_ar_glass_init(); // 这里调用我们在 ui_ar_glass.c 里写的界面初始化函数
        ui_menu_screen_init(); // 初始化菜单界面
        ui_novel_screen_init();
        lv_scr_load(ui_novel_screen);
        lvgl_port_unlock(); // 别忘了解锁，否则屏幕不刷新

    }
    // 5. 初始化硬件 I2C 总线
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C 硬件总线初始化完毕！");

    // 6. 连接 Wi-Fi (依赖前面的 NVS 初始化)
    wifi_init_sta();

    // 7. 初始化传感器
    if (mpu6050_init_all() == ESP_OK) {
        ESP_LOGI(TAG, "MPU6050 唤醒成功！");
    }
    if (max30105_init(I2C_MASTER_NUM) == ESP_OK) {
        ESP_LOGI(TAG, "MAX30105 配置成功！");
    }

    // 8. 初始化音频驱动
    if (audio_driver_init() != ESP_OK) {
        printf("❌ 音频模块初始化失败！请检查日志。\n");
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
    // 10. 创建传感器读取任务
    // ⚠️ 注意：前提是你已经在其他文件实现了 read_mpu6050_task，否则编译会报错找不到该函数
    xTaskCreate(read_mpu6050_task, "read_mpu6050_task", 4096, NULL, 5, NULL);
    xTaskCreate(read_max30105_task, "read_max30105_task", 4096, NULL, 6, NULL);
    
    // 11. 主循环挂起
    while (1) {
        app_mqtt_publish("home/status/sensor", "TEMP:25C");
        if (lvgl_port_lock(0))
        {
            // 获取当前选中的索引
            uint16_t cur_opt = lv_roller_get_selected(menu_roller);
            // 往下滚一项 (带动画)
            lv_roller_set_selected(menu_roller, (cur_opt + 1)%4, LV_ANIM_ON);
            
            lvgl_port_unlock();
        }
        novel_scroll_one_line();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
