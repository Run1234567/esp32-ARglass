// ============================================================
// gps.c
// J.A.R.V.I.S. AR 智能眼镜 —— GPS 模块驱动
// ============================================================
// 硬件：ATGM336H 北斗+GPS 双模定位模块
// 接线：TX → GPIO 18 (ESP32 RX)，RX → GPIO 17 (ESP32 TX)
// 波特率：9600
// ============================================================

#include "gps.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "GPS";

// ============================================================
//   硬件配置
// ============================================================
#define GPS_TX_PIN      17
#define GPS_RX_PIN      18
#define GPS_UART_NUM    UART_NUM_2
#define GPS_BAUD_RATE   9600
#define GPS_BUF_SIZE    512
#define GPS_TASK_STACK  4096
#define GPS_TASK_PRIO   4

// ============================================================
//   模块内部变量
// ============================================================
static TaskHandle_t gps_task_handle = NULL;
static StackType_t *gps_stack = NULL;    // Bug3 修复：提升为全局，deinit 时可释放
static StaticTask_t *gps_tcb = NULL;     // Bug3 修复：同上
static bool is_initialized = false;

static volatile gps_data_t s_gps_data = {0};

// ============================================================
//   Bug1 修复：nmea_field 改为写入调用者提供的缓冲区
//   避免多个字段共享同一个 static 指针导致数据被覆盖
// ============================================================
static void nmea_field(const char *sentence, int index, char *out, int max_len) {
    out[0] = '\0';
    int comma_count = 0;
    const char *start = sentence;

    for (const char *p = sentence; *p; p++) {
        if (*p == ',') {
            comma_count++;
            if (comma_count == index) {
                start = p + 1;
            } else if (comma_count == index + 1) {
                int len = p - start;
                if (len >= max_len) len = max_len - 1;
                memcpy(out, start, len);
                out[len] = '\0';
                return;
            }
        }
    }
    if (comma_count == index) {
        const char *end = strchr(start, '*');
        if (!end) end = start + strlen(start);
        int len = end - start;
        if (len >= max_len) len = max_len - 1;
        memcpy(out, start, len);
        out[len] = '\0';
    }
}

// NMEA 校验和验证
static bool nmea_checksum(const char *sentence) {
    if (sentence[0] != '$') return false;

    uint8_t calc = 0;
    const char *p = sentence + 1;

    while (*p && *p != '*') {
        calc ^= (uint8_t)*p;
        p++;
    }

    if (*p != '*' || *(p + 1) == '\0') return false;

    uint8_t received = (uint8_t)strtol(p + 1, NULL, 16);
    return calc == received;
}

// NMEA 经纬度转十进制度
// NMEA 格式始终是 (D)DDMM.MMMM，除以 100 分离度和分
static double nmea_to_decimal(const char *nmea_val, const char dir) {
    if (!nmea_val || nmea_val[0] == '\0') return 0.0;

    double raw = atof(nmea_val);
    double degrees = (int)(raw / 100.0);
    double minutes = raw - degrees * 100.0;
    double decimal = degrees + minutes / 60.0;

    if (dir == 'S' || dir == 'W') decimal = -decimal;
    return decimal;
}

// ============================================================
//   解析 NMEA 语句（Bug1 修复：使用局部数组接收字段）
// ============================================================
static void parse_nmea(const char *sentence) {
    if (!nmea_checksum(sentence)) return;

    // ---- $GPRMC / $GNRMC ----
    if (strncmp(sentence + 1, "GPRMC", 5) == 0 ||
        strncmp(sentence + 1, "GNRMC", 5) == 0) {

        char status[4];
        nmea_field(sentence, 2, status, sizeof(status));
        if (status[0] != 'A') {
            s_gps_data.valid = false;
            return;
        }

        char time_f[16], lat_f[16], lat_dir[4], lon_f[16], lon_dir[4], spd_f[16], date_f[16];
        nmea_field(sentence, 1, time_f, sizeof(time_f));
        nmea_field(sentence, 3, lat_f, sizeof(lat_f));
        nmea_field(sentence, 4, lat_dir, sizeof(lat_dir));
        nmea_field(sentence, 5, lon_f, sizeof(lon_f));
        nmea_field(sentence, 6, lon_dir, sizeof(lon_dir));
        nmea_field(sentence, 7, spd_f, sizeof(spd_f));
        nmea_field(sentence, 9, date_f, sizeof(date_f));

        if (time_f[0]) {
            char buf[11] = {0};
            if (strlen(time_f) >= 6) {
                int hour = (time_f[0] - '0') * 10 + (time_f[1] - '0');
                hour = (hour + 8) % 24; // UTC+8 北京时间
                buf[0] = (hour / 10) + '0';
                buf[1] = (hour % 10) + '0';
                buf[2] = ':';
                buf[3] = time_f[2]; buf[4] = time_f[3]; buf[5] = ':';
                buf[6] = time_f[4]; buf[7] = time_f[5]; buf[8] = '\0';
            }
            memcpy((void*)s_gps_data.utc_time, buf, sizeof(buf));
        }

        if (date_f[0]) {
            char buf[11] = {0};
            if (strlen(date_f) >= 6) {
                buf[0] = date_f[0]; buf[1] = date_f[1]; buf[2] = '/';
                buf[3] = date_f[2]; buf[4] = date_f[3]; buf[5] = '/';
                buf[6] = date_f[4]; buf[7] = date_f[5]; buf[8] = '\0';
            }
            memcpy((void*)s_gps_data.utc_date, buf, sizeof(buf));
        }

        if (lat_f[0] && lat_dir[0] && lon_f[0] && lon_dir[0]) {
            s_gps_data.latitude  = nmea_to_decimal(lat_f, lat_dir[0]);
            s_gps_data.longitude = nmea_to_decimal(lon_f, lon_dir[0]);
        }

        if (spd_f[0]) {
            s_gps_data.speed_knots = atof(spd_f);
            s_gps_data.speed_kmh   = s_gps_data.speed_knots * 1.852f;
        }

        s_gps_data.valid = true;
    }
    // ---- $GPGGA / $GNGGA ----
    else if (strncmp(sentence + 1, "GPGGA", 5) == 0 ||
             strncmp(sentence + 1, "GNGGA", 5) == 0) {

        char sat_f[8], alt_f[16];
        nmea_field(sentence, 7, sat_f, sizeof(sat_f));
        nmea_field(sentence, 9, alt_f, sizeof(alt_f));

        if (sat_f[0]) {
            s_gps_data.satellites = atoi(sat_f);
        }
        if (alt_f[0]) {
            s_gps_data.altitude = atof(alt_f);
        }
    }
}

// ============================================================
//   GPS 后台解析任务
// ============================================================
static void gps_read_task(void *arg) {
    uint8_t *buf = malloc(GPS_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "缓冲区分配失败！");
        vTaskDelete(NULL);
        return;
    }

    static char sentence[256];
    static int  sentence_len = 0;
    int print_counter = 0;

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, buf, GPS_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            char c = (char)buf[i];

            if (c == '$') {
                sentence_len = 0;
                sentence[sentence_len++] = c;
            }
            else if (sentence_len > 0 && sentence_len < (int)sizeof(sentence) - 1) {
                sentence[sentence_len++] = c;

                if (c == '\n' || c == '\r') {
                    sentence[sentence_len] = '\0';
                    parse_nmea(sentence);
                    sentence_len = 0;
                }
            }
        }

        // 每 5 秒打印一次
        print_counter++;
        if (print_counter >= 50) {
            print_counter = 0;
            if (s_gps_data.valid) {
                ESP_LOGI(TAG, "LAT:%.6f LON:%.6f SPD:%.1fkm/h SAT:%d ALT:%.1fm %s",
                         s_gps_data.latitude, s_gps_data.longitude,
                         s_gps_data.speed_kmh, s_gps_data.satellites,
                         s_gps_data.altitude, s_gps_data.utc_time);
            } else {
                ESP_LOGW(TAG, "搜星中... (sat:%d)", s_gps_data.satellites);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(buf);
}

// ============================================================
//   初始化
// ============================================================
esp_err_t gps_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "GPS 已初始化，跳过");
        return ESP_OK;
    }

    uart_config_t uart_config = {
        .baud_rate  = GPS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err;

    err = uart_driver_install(GPS_UART_NUM, GPS_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 驱动安装失败: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(GPS_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 参数配置失败: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 引脚设置失败: %s", esp_err_to_name(err));
        return err;
    }

    // Bug3 修复：全局指针，deinit 时可释放
    gps_stack = heap_caps_malloc(GPS_TASK_STACK, MALLOC_CAP_SPIRAM);
    gps_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!gps_stack || !gps_tcb) {
        ESP_LOGE(TAG, "GPS 任务内存分配失败！");
        if (gps_stack) { heap_caps_free(gps_stack); gps_stack = NULL; }
        if (gps_tcb) { heap_caps_free(gps_tcb); gps_tcb = NULL; }
        return ESP_ERR_NO_MEM;
    }

    // Bug2 修复：ESP-IDF 的 xTaskCreateStatic 栈大小单位是字节，不要除以 sizeof(StackType_t)
    gps_task_handle = xTaskCreateStaticPinnedToCore(
        gps_read_task, "gps_task", GPS_TASK_STACK,
        NULL, GPS_TASK_PRIO, gps_stack, gps_tcb, 0
    );

    if (!gps_task_handle) {
        ESP_LOGE(TAG, "GPS 任务创建失败！");
        heap_caps_free(gps_stack); gps_stack = NULL;
        heap_caps_free(gps_tcb); gps_tcb = NULL;
        return ESP_FAIL;
    }

    is_initialized = true;
    ESP_LOGI(TAG, "GPS 初始化完成 (UART2, TX:%d RX:%d)", GPS_TX_PIN, GPS_RX_PIN);
    return ESP_OK;
}

// ============================================================
//   获取最新 GPS 数据
// ============================================================
gps_data_t gps_get_data(void) {
    return (gps_data_t)s_gps_data;
}

// ============================================================
//   释放 GPS 资源（Bug3 修复：释放 PSRAM 内存）
// ============================================================
esp_err_t gps_deinit(void) {
    if (!is_initialized) return ESP_OK;

    if (gps_task_handle) {
        vTaskDelete(gps_task_handle);
        gps_task_handle = NULL;
    }
    if (gps_stack) {
        heap_caps_free(gps_stack);
        gps_stack = NULL;
    }
    if (gps_tcb) {
        heap_caps_free(gps_tcb);
        gps_tcb = NULL;
    }

    uart_driver_delete(GPS_UART_NUM);
    is_initialized = false;
    memset((void*)&s_gps_data, 0, sizeof(s_gps_data));

    ESP_LOGI(TAG, "GPS 已释放");
    return ESP_OK;
}
