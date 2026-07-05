#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "my_font_cn_16.h"  // 中文字体
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// ================= 核心网络配置区 =================
#define WIFI_SSID       "RUN"
#define WIFI_PASS       "88888888"

#define SERVER_IP       "124.220.224.189"

// 【非常重要】当前板子是 ESP32-A，使用 8888/8889
// 如果你烧录另一块板子 (ESP32-B)，请改成 9998 和 9999
#define SERVER_PORT_IN  9998  // 麦克风录音发到服务器的这个端口
#define SERVER_PORT_OUT 9999  // 从服务器的这个端口拉取对方的声音

// ================= 新增信令配置 =================
#define SERVER_PORT_SIG 7777
#define DEVICE_NAME     "B"  // 【非常重要】烧录第二块板子时，请改为 "B"
// ===============================================

#define I2S_SCK_PIN    41
#define I2S_WS_PIN     42
#define I2S_SD_PIN     2
#define SAMPLE_RATE    16000

#define SPK_BCLK_PIN   39
#define SPK_LRC_PIN    40
#define SPK_DIN_PIN    38

#define WS2812_PIN     48
#define WS2812_NUM     1

#define PIN_CAM_XCLK   15
#define PIN_CAM_SIOD   4
#define PIN_CAM_SIOC   5
#define PIN_CAM_Y9     16
#define PIN_CAM_Y8     17
#define PIN_CAM_Y7     18
#define PIN_CAM_Y6     12
#define PIN_CAM_Y5     10
#define PIN_CAM_Y4     8
#define PIN_CAM_Y3     9
#define PIN_CAM_Y2     11
#define PIN_CAM_VSYNC  6
#define PIN_CAM_HREF   7
#define PIN_CAM_PCLK   13

#define TFT_SCK_PIN    20
#define TFT_MOSI_PIN   19
#define TFT_CS_PIN     5
#define TFT_DC_PIN     47
#define TFT_RST_PIN    14
// 注意：引脚 1 现在用于摇杆 ADC，不再是背光引脚

#define TFT_WIDTH      128
#define TFT_HEIGHT     160

static const char *TAG = "JARVIS_COMMS";

// ================= 函数提前声明 =================
void update_status_text(const char *text);
void update_hint_text(const char *text);
void update_contact_selection(void);  // 联系人选择更新函数
void send_signaling_msg(const char *msg);  // 信令发送函数
// =================================================

// ================= LVGL 全局屏幕对象与状态 =================
lv_obj_t *scr_main;      // 0: 主界面
lv_obj_t *scr_contacts;  // 1: 联系人界面
lv_obj_t *scr_waiting;   // 2: 等待对方接听界面
lv_obj_t *scr_incoming;  // 3: 收到来电界面
lv_obj_t *scr_calling;   // 4: 通话中界面

lv_obj_t *label_contact1; // 联系人1标签
lv_obj_t *label_contact2; // 联系人2标签
lv_obj_t *label_call_tip; // 呼叫界面的提示文字

int current_screen = 0;   // 0=主界面, 1=联系人, 2=等待对方接听, 3=收到来电, 4=通话中
int selected_contact = 0; // 0=AR眼镜端, 1=客户手机端

// ================= 对讲通话控制 =================
volatile bool is_calling = false; // 通话开关，默认关闭
TaskHandle_t tx_task_handle = NULL;
TaskHandle_t rx_task_handle = NULL;

// ================= 信令控制 =================
int sig_sock = -1; // 信令 Socket 句柄

// ADC 句柄用于摇杆
adc_oneshot_unit_handle_t adc1_handle;

i2s_chan_handle_t rx_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

static camera_config_t camera_config = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = PIN_CAM_XCLK,
    .pin_sccb_sda = PIN_CAM_SIOD,
    .pin_sccb_scl = PIN_CAM_SIOC,
    .pin_d7       = PIN_CAM_Y9,
    .pin_d6       = PIN_CAM_Y8,
    .pin_d5       = PIN_CAM_Y7,
    .pin_d4       = PIN_CAM_Y6,
    .pin_d3       = PIN_CAM_Y5,
    .pin_d2       = PIN_CAM_Y4,
    .pin_d1       = PIN_CAM_Y3,
    .pin_d0       = PIN_CAM_Y2,
    .pin_vsync    = PIN_CAM_VSYNC,
    .pin_href     = PIN_CAM_HREF,
    .pin_pclk     = PIN_CAM_PCLK,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size   = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count     = 1,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY
};

// ================= 1. WS2812 LED =================
static led_strip_handle_t configure_led(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_PIN,
        .max_leds = WS2812_NUM,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    return led_strip;
}

static void set_rainbow_color(led_strip_handle_t strip, uint8_t pos) {
    uint8_t r = 0, g = 0, b = 0;
    if (pos < 85) { r = pos * 3; g = 255 - pos * 3; b = 0; }
    else if (pos < 170) { pos -= 85; r = 255 - pos * 3; g = 0; b = pos * 3; }
    else { pos -= 170; r = 0; g = pos * 3; b = 255 - pos * 3; }
    r /= 10; g /= 10; b /= 10;
    led_strip_set_pixel(strip, 0, g, r, b);
}

static void rainbow_task(void *arg) {
    led_strip_handle_t led_strip = (led_strip_handle_t)arg;
    uint8_t color_pos = 0;
    while (1) {
        set_rainbow_color(led_strip, color_pos);
        led_strip_refresh(led_strip);
        color_pos++;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// ================= 2. 麦克风 I2S 初始化 =================
void init_microphone() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = I2S_SCK_PIN, .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED, .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

// ================= 3. 扬声器 I2S 初始化 =================
void init_speaker() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRC_PIN,
            .dout = SPK_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

// ================= 摇杆 ADC 初始化 =================
void init_joystick() {
    ESP_LOGI(TAG, "初始化摇杆 ADC (引脚 1 和 3)...");

    // 1. 初始化 ADC1 单元
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    // 2. 配置通道参数：12位分辨率 (0-4095)，12dB 衰减 (测量范围 0~3.3V)
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,  // 新版 ESP-IDF 使用 DB_12 替代 DB_11
    };

    // 引脚 1 对应 ADC_CHANNEL_0
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));
    // 引脚 3 对应 ADC_CHANNEL_2
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_2, &config));
}

// ================= 摇杆数据读取与界面控制任务 =================
static void joystick_task(void *arg) {
    int x_raw, y_raw;
    int prev_x_state = 0;
    int prev_y_state = 0;

    while (1) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &x_raw));
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_2, &y_raw));

        int curr_x_state = 0;
        if (x_raw > 3000) curr_x_state = 1;       // 向上划
        else if (x_raw < 1000) curr_x_state = -1; // 向下划

        int curr_y_state = 0;
        if (y_raw > 3000) curr_y_state = 1;       // 向右划
        else if (y_raw < 1000) curr_y_state = -1; // 向左划

        // ================= X 轴：上下滑动选择 =================
        if (prev_x_state == 0 && curr_x_state != 0) {
            if (current_screen == 1) { // 只有在联系人界面才处理上下选择
                if (curr_x_state == 1) {
                    selected_contact = 0; // 选上方的 AR 眼镜
                    update_contact_selection();
                } else if (curr_x_state == -1) {
                    selected_contact = 1; // 选下方的 客户手机
                    update_contact_selection();
                }
            }
        }

        // ================= Y 轴：左右滑动切屏 =================
        if (prev_y_state == 0 && curr_y_state != 0) {

            if (curr_y_state == 1) { // 向右推
                if (current_screen == 0) {
                    current_screen = 1;
                    lv_scr_load_anim(scr_contacts, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                }
                else if (current_screen == 1) {
                    // 拨号：发 CALL，进入"等待接听"界面 (不开启音频)
                    send_signaling_msg("CALL\n");
                    current_screen = 2;
                    lv_scr_load_anim(scr_waiting, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                }
                else if (current_screen == 3) {
                    // 接听来电：发 ACCEPT，进入"通话中"界面，并开启音频
                    send_signaling_msg("ACCEPT\n");
                    current_screen = 4;
                    is_calling = true; // 🌟 开启对讲开关！
                    lv_scr_load_anim(scr_calling, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
                }
            }
            else if (curr_y_state == -1) { // 向左推
                if (current_screen == 1) {
                    current_screen = 0;
                    lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                }
                else if (current_screen == 2) {
                    // 拨号中途取消：发 HANGUP，退回联系人
                    send_signaling_msg("HANGUP\n");
                    current_screen = 1;
                    lv_scr_load_anim(scr_contacts, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                }
                else if (current_screen == 3) {
                    // 拒绝来电：发 HANGUP，退回联系人
                    send_signaling_msg("HANGUP\n");
                    current_screen = 1;
                    lv_scr_load_anim(scr_contacts, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                }
                else if (current_screen == 4) {
                    // 通话中挂断：发 HANGUP，关音频，退回联系人
                    send_signaling_msg("HANGUP\n");
                    is_calling = false; // 🌟 关闭对讲开关！
                    current_screen = 1;
                    lv_scr_load_anim(scr_contacts, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                }
            }
        }

        prev_x_state = curr_x_state;
        prev_y_state = curr_y_state;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ================= 信令发送辅助函数 =================
void send_signaling_msg(const char *msg) {
    if (sig_sock >= 0) {
        send(sig_sock, msg, strlen(msg), 0);
    }
}

// ================= 任务1: 网络推流任务 (麦克风 -> 服务器) =================
static void megaphone_task(void *arg) {
    size_t bytes_read = 0;
    #define CHUNK_SAMPLES 512

    int32_t *mic_buff = (int32_t *)malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t));
    int16_t *spk_buff = (int16_t *)malloc(CHUNK_SAMPLES * sizeof(int16_t));

    if (!mic_buff || !spk_buff) {
        ESP_LOGE(TAG, "扩音器内存分配失败！");
        vTaskDelete(NULL);
        return;
    }

    int sock = -1;
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT_IN);  // 发送使用 IN 端口

    ESP_LOGI(TAG, "🎤 推流任务准备就绪！");

    while (1) {
        // 🌟 待机模式：不在通话中时休眠，不抢占 socket
        if (!is_calling) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 自动重连机制
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                ESP_LOGI(TAG, "尝试连接发送通道 %d...", SERVER_PORT_IN);
                if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
                    close(sock);
                    sock = -1;
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                ESP_LOGI(TAG, "✅ 发送通道连接成功！");
            }
        }

        if (i2s_channel_read(rx_handle, mic_buff, CHUNK_SAMPLES * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {
            int frames = bytes_read / (2 * sizeof(int32_t));

            for (int i = 0; i < frames; i++) {
                int32_t sample = mic_buff[i * 2] >> 16;
                sample = sample * 4;
                if (sample > 32767)  sample = 32767;
                if (sample < -32768) sample = -32768;
                spk_buff[i] = (int16_t)sample;
            }

            // 发送到云服务器 (注意：移除了本地 I2S 写入，避免听到自己的回音)
            if (sock >= 0) {
                int err = send(sock, spk_buff, frames * sizeof(int16_t), 0);
                if (err < 0) {
                    ESP_LOGE(TAG, "❌ 发送断开，准备重连...");
                    close(sock);
                    sock = -1;
                }
            }
        }
    }
}

// ================= 任务2: 接收播放任务 (服务器 -> 扬声器) =================
static void stream_receiver_task(void *arg) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT_OUT); // 接收使用 OUT 端口

    while (1) {
        // 🌟 待机模式：不在通话中时休眠，不抢占 socket
        if (!is_calling) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "尝试连接接收通道 %d...", SERVER_PORT_OUT);
        while (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
            ESP_LOGE(TAG, "接收通道连接失败，2秒后重试...");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        ESP_LOGI(TAG, "✅ 接收通道连接成功！");
        uint8_t recv_buff[1024];
        size_t bytes_written;

        while (1) {
            // 🌟 通话中才接收数据
            if (!is_calling) {
                break;
            }

            int len = recv(sock, recv_buff, sizeof(recv_buff), 0);
            if (len > 0) {
                // ================= 开始音量放大算法 =================
                // 1. 把接收到的 8-bit 字节流当作 16-bit 的声音采样点来处理
                int16_t *pcm_data = (int16_t *)recv_buff;
                int sample_count = len / 2; // 计算有多少个采样点

                // 2. 遍历每一个声音点
                for (int i = 0; i < sample_count; i++) {
                    // 先用 32 位的大容器装起来，防止乘 4 之后撑爆
                    int32_t sample = (int32_t)pcm_data[i];

                    // 🌟 放大 8 倍！
                    sample = sample * 8;

                    // 防爆音保护：超过物理极限的声音，强行削平（硬截断）
                    if (sample > 32767) sample = 32767;
                    if (sample < -32768) sample = -32768;

                    // 把处理好的饱满声音塞回原来的数组
                    pcm_data[i] = (int16_t)sample;
                }
                // ====================================================

                // 将放大后的声音通过扬声器放出来
                i2s_channel_write(tx_handle, recv_buff, len, &bytes_written, portMAX_DELAY);
            } else {
                ESP_LOGE(TAG, "❌ 接收断开，准备重连...");
                break; // 跳出内层循环，重新 socket connect
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================= 信令监听任务 (TCP 客户端) =================
static void signaling_task(void *arg) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT_SIG);

    while (1) {
        sig_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sig_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "尝试连接信令服务器...");
        if (connect(sig_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == 0) {
            ESP_LOGI(TAG, "✅ 信令服务器连接成功！");

            // 登录
            char login_msg[32];
            sprintf(login_msg, "LOGIN:%s\n", DEVICE_NAME);
            send(sig_sock, login_msg, strlen(login_msg), 0);

            char rx_buffer[128];
            while (1) {
                int len = recv(sig_sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
                if (len > 0) {
                    rx_buffer[len] = 0; // 字符串结束符
                    ESP_LOGI(TAG, "收到信令: %s", rx_buffer);

                    if (strstr(rx_buffer, "RING")) {
                        // 有人打来了
                        if (current_screen == 1 || current_screen == 0) {
                            current_screen = 3; // 收到来电界面
                            lv_scr_load_anim(scr_incoming, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
                        }
                    }
                    else if (strstr(rx_buffer, "ACCEPTED")) {
                        // 对方接听了
                        if (current_screen == 2) {
                            current_screen = 4; // 通话中界面
                            is_calling = true;  // 🌟 真正开启音频收发！
                            lv_scr_load_anim(scr_calling, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
                        }
                    }
                    else if (strstr(rx_buffer, "HANGUP")) {
                        // 对方挂断/拒绝
                        is_calling = false; // 关闭音频
                        current_screen = 1; // 回到联系人界面
                        lv_scr_load_anim(scr_contacts, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
                    }
                } else {
                    ESP_LOGE(TAG, "信令断开");
                    break;
                }
            }
        }

        if (sig_sock != -1) {
            close(sig_sock);
            sig_sock = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(2000)); // 断线重连
    }
}

// ================= 屏幕与 LVGL 初始化模块 =================
void init_tft() {
    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SCK_PIN,
        .mosi_io_num = TFT_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * 16 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_DC_PIN,
        .cs_gpio_num = TFT_CS_PIN,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // 注意：引脚 1 现在用于摇杆 ADC，背光控制已移除
    // 如果需要背光控制，请使用其他引脚
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t) drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_tick_task(void *arg) { lv_tick_inc(2); }

void init_lvgl(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io) {
    lv_init();
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = notify_lvgl_flush_ready };
    #define LVGL_BUFFER_SIZE (TFT_WIDTH * TFT_HEIGHT / 10)
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LVGL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, LVGL_BUFFER_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TFT_WIDTH;
    disp_drv.ver_res = TFT_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel;
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, &disp_drv);
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t lvgl_tick_timer_args = { .callback = &lvgl_tick_task, .name = "lvgl_tick" };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2 * 1000));
}

// ================= 更新联系人选中状态 =================
void update_contact_selection() {
    if (selected_contact == 0) {
        lv_label_set_text(label_contact1, "> AR眼镜端");
        lv_obj_set_style_text_color(label_contact1, lv_color_hex(0xFFFFFF), 0);

        lv_label_set_text(label_contact2, "  客户手机端");
        lv_obj_set_style_text_color(label_contact2, lv_color_hex(0x888888), 0);
    } else {
        lv_label_set_text(label_contact1, "  AR眼镜端");
        lv_obj_set_style_text_color(label_contact1, lv_color_hex(0x888888), 0);

        lv_label_set_text(label_contact2, "> 客户手机端");
        lv_obj_set_style_text_color(label_contact2, lv_color_hex(0xFFFFFF), 0);
    }
}

// ================= 构建五屏 UI =================
void build_ui(void) {
    // ----------------- 1. 主界面 -----------------
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x000000), 0);

    lv_obj_t *label1 = lv_label_create(scr_main);
    lv_obj_set_style_text_font(label1, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label1, lv_color_hex(0x00A8FF), 0);
    lv_label_set_text(label1, "智能AR眼镜系统");
    lv_obj_align(label1, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *label2 = lv_label_create(scr_main);
    lv_obj_set_style_text_font(label2, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label2, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(label2, "通话功能演示端");
    lv_obj_align(label2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label3 = lv_label_create(scr_main);
    lv_obj_set_style_text_font(label3, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label3, lv_color_hex(0xFFFF00), 0);
    lv_label_set_text(label3, "右划进入通话功能");
    lv_obj_align(label3, LV_ALIGN_BOTTOM_MID, 0, -20);

    // ----------------- 2. 联系人界面 -----------------
    scr_contacts = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_contacts, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr_contacts);
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00A8FF), 0);
    lv_label_set_text(title, "请选择联系人");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    label_contact1 = lv_label_create(scr_contacts);
    lv_obj_set_style_text_font(label_contact1, &my_font_cn_16, 0);
    lv_obj_align(label_contact1, LV_ALIGN_CENTER, 0, -15);

    label_contact2 = lv_label_create(scr_contacts);
    lv_obj_set_style_text_font(label_contact2, &my_font_cn_16, 0);
    lv_obj_align(label_contact2, LV_ALIGN_CENTER, 0, 15);

    // 初始化光标状态
    update_contact_selection();

    // ----------------- 3. 等待对方接听界面 -----------------
    scr_waiting = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_waiting, lv_color_hex(0x000000), 0);

    lv_obj_t *label_wait = lv_label_create(scr_waiting);
    lv_obj_set_style_text_font(label_wait, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_wait, lv_color_hex(0x00A8FF), 0);
    lv_label_set_text(label_wait, "正在呼叫对方...\n等待接听");
    lv_obj_align(label_wait, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label_wait_cancel = lv_label_create(scr_waiting);
    lv_obj_set_style_text_font(label_wait_cancel, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_wait_cancel, lv_color_hex(0xFF0000), 0);
    lv_label_set_text(label_wait_cancel, "< 左滑取消");
    lv_obj_align(label_wait_cancel, LV_ALIGN_BOTTOM_MID, 0, -20);

    // ----------------- 4. 收到来电界面 -----------------
    scr_incoming = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_incoming, lv_color_hex(0x000000), 0);

    lv_obj_t *label_in = lv_label_create(scr_incoming);
    lv_obj_set_style_text_font(label_in, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_in, lv_color_hex(0xFFFF00), 0);
    lv_label_set_text(label_in, "收到来电请求！");
    lv_obj_align(label_in, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *label_in_op = lv_label_create(scr_incoming);
    lv_obj_set_style_text_font(label_in_op, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_in_op, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(label_in_op, "左滑拒绝   右滑接听");
    lv_obj_align(label_in_op, LV_ALIGN_BOTTOM_MID, 0, -20);

    // ----------------- 5. 通话中界面 -----------------
    scr_calling = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_calling, lv_color_hex(0x000000), 0);

    label_call_tip = lv_label_create(scr_calling);
    lv_obj_set_style_text_font(label_call_tip, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_call_tip, lv_color_hex(0x00FF00), 0);
    lv_label_set_text(label_call_tip, "通话中...");
    lv_obj_align(label_call_tip, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label_hangup = lv_label_create(scr_calling);
    lv_obj_set_style_text_font(label_hangup, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(label_hangup, lv_color_hex(0xFF0000), 0);
    lv_label_set_text(label_hangup, "< 左滑挂断");
    lv_obj_align(label_hangup, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_scr_load(scr_main);
}

// ================= 动态更新中文显示 =================
// 全局标签指针，用于在其他任务中更新显示
static lv_obj_t *g_status_label = NULL;
static lv_obj_t *g_hint_label = NULL;

/**
 * @brief 更新状态标签的中文文本
 * @param text 要显示的中文文本
 */
void update_status_text(const char *text) {
    if (g_status_label != NULL) {
        lv_label_set_text(g_status_label, text);
    }
}

/**
 * @brief 更新提示标签的中文文本
 * @param text 要显示的中文文本
 */
void update_hint_text(const char *text) {
    if (g_hint_label != NULL) {
        lv_label_set_text(g_hint_label, text);
    }
}

// ================= WiFi 初始化 =================
static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Wi-Fi 正在连接...");
    vTaskDelay(pdMS_TO_TICKS(5000)); // 等待分配 IP
}

// ================= 主函数 =================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    led_strip_handle_t led_strip = configure_led();
    led_strip_clear(led_strip);
    xTaskCreate(rainbow_task, "rainbow", 2048, (void *)led_strip, 5, NULL);

    esp_err_t err = esp_camera_init(&camera_config);
    if (err == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) { s->set_vflip(s, 0); s->set_hmirror(s, 0); }
    }

    init_tft();
    init_lvgl(panel_handle, io_handle);

    // 🌟 构建双屏幕 UI
    build_ui();

    init_microphone();
    init_speaker();

    // 🌟 新增：初始化并启动摇杆
    init_joystick();
    xTaskCreate(joystick_task, "joystick", 4096, NULL, 5, NULL);

    // 🌟 新增：启动信令监听任务
    xTaskCreate(signaling_task, "signaling", 4096, NULL, 5, NULL);

    // 🌟 同时启动发声和收声任务！
    xTaskCreate(megaphone_task, "megaphone", 8192, NULL, 5, NULL);
    xTaskCreate(stream_receiver_task, "stream_recv", 8192, NULL, 5, NULL);

    while (1) {
        lv_timer_handler();
        // 确保延时至少为 10ms (在 100Hz 系统下保证释放 CPU)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}