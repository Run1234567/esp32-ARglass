// ============================================================
// gps.c
// J.A.R.V.I.S. AR 智能眼镜 —— GPS 模块驱动
// ============================================================
// 硬件：ATGM336H 北斗+GPS 双模定位模块
// 接线：TX → GPIO 18 (ESP32 RX)，RX → GPIO 17 (ESP32 TX)
// 波特率：9600
//
// 工作模式：后台任务持续读取 UART 数据，解析 NMEA 协议，
//           提取经纬度、速度、海拔、卫星数等信息存入缓存。
//           其他模块随时调用 gps_get_data() 获取最新定位。
// ============================================================

#include "gps.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "GPS";

// ============================================================
//   硬件配置
// ============================================================
#define GPS_TX_PIN      17              // ESP32 TX → GPS RX
#define GPS_RX_PIN      18              // ESP32 RX → GPS TX
#define GPS_UART_NUM    UART_NUM_2      // 使用 UART2（UART1 已被 my_uart 占用）
#define GPS_BAUD_RATE   9600            // ATGM336H 默认波特率
#define GPS_BUF_SIZE    1024             // UART 接收缓冲区
#define GPS_TASK_STACK  4096            // 任务栈大小
#define GPS_TASK_PRIO   4               // 任务优先级

// ============================================================
//   模块内部变量
// ============================================================
static TaskHandle_t gps_task_handle = NULL;
static bool is_initialized = false;

// 最新 GPS 数据缓存（volatile 保证多任务可见性）
static volatile gps_data_t s_gps_data = {0};

// ============================================================
//   NMEA 解析辅助函数
// ============================================================

// 从 NMEA 语句中提取第 index 个字段（逗号分隔）
// 返回：指向字段内容的指针，或 NULL（字段不存在）
static const char *nmea_field(const char *sentence, int index) {
    int comma_count = 0;
    const char *start = sentence;

    for (const char *p = sentence; *p; p++) {
        if (*p == ',') {
            comma_count++;
            if (comma_count == index) {
                start = p + 1;
            } else if (comma_count == index + 1) {
                static char field[32];
                int len = p - start;
                if (len >= (int)sizeof(field)) len = sizeof(field) - 1;
                memcpy(field, start, len);
                field[len] = '\0';
                return field;
            }
        }
    }
    // 最后一个字段（没有逗号结尾，只有 * 或结尾）
    if (comma_count == index) {
        static char field[32];
        const char *end = strchr(start, '*');
        if (!end) end = start + strlen(start);
        int len = end - start;
        if (len >= (int)sizeof(field)) len = sizeof(field) - 1;
        memcpy(field, start, len);
        field[len] = '\0';
        return field;
    }
    return NULL;
}

// NMEA 校验和验证
// 格式：$xxxxx*HH，HH 是前面所有字符异或的结果
static bool nmea_checksum(const char *sentence) {
    if (sentence[0] != '$') return false;

    uint8_t calc = 0;
    const char *p = sentence + 1; // 跳过 '$'

    while (*p && *p != '*') {
        calc ^= (uint8_t)*p;
        p++;
    }

    if (*p != '*' || *(p + 1) == '\0') return false;

    uint8_t received = (uint8_t)strtol(p + 1, NULL, 16);
    return calc == received;
}

// 将 NMEA 格式的经纬度转换为十进制度
// NMEA 格式：ddmm.mmmm（纬度）或 dddmm.mmmm（经度）
static double nmea_to_decimal(const char *nmea_val, const char dir) {
    if (!nmea_val || nmea_val[0] == '\0') return 0.0;

    double raw = atof(nmea_val);

    // 提取度数：纬度前2位，经度前3位
    int deg_digits = (dir == 'N' || dir == 'S') ? 2 : 3;
    double divisor = pow(10.0, deg_digits);
    double degrees = (int)(raw / divisor);
    double minutes = raw - degrees * divisor;

    double decimal = degrees + minutes / 60.0;

    // 南纬和西经为负数
    if (dir == 'S' || dir == 'W') decimal = -decimal;

    return decimal;
}

// ============================================================
//   解析一条 NMEA 语句
// ============================================================
static void parse_nmea(const char *sentence) {
    // 校验和验证
    if (!nmea_checksum(sentence)) return;

    // ---- 解析 $GPRMC（推荐最小定位信息） ----
    // 格式：$GPRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a*hh
    // 字段：  0       1    2     3   4      5   6   7     8     9  10
    if (strncmp(sentence + 1, "GPRMC", 5) == 0 ||
        strncmp(sentence + 1, "GNRMC", 5) == 0) {

        const char *status = nmea_field(sentence, 2);
        if (!status || status[0] != 'A') {
            // 'V' = 无效定位
            s_gps_data.valid = false;
            return;
        }

        const char *time_f  = nmea_field(sentence, 1);
        const char *lat_f   = nmea_field(sentence, 3);
        const char *lat_dir = nmea_field(sentence, 4);
        const char *lon_f   = nmea_field(sentence, 5);
        const char *lon_dir = nmea_field(sentence, 6);
        const char *spd_f   = nmea_field(sentence, 7);
        const char *date_f  = nmea_field(sentence, 9);

        if (time_f && time_f[0]) {
            // 格式化时间 HH:MM:SS
            char buf[11] = {0};
            int len = strlen(time_f);
            if (len >= 6) {
                buf[0] = time_f[0]; buf[1] = time_f[1]; buf[2] = ':';
                buf[3] = time_f[2]; buf[4] = time_f[3]; buf[5] = ':';
                buf[6] = time_f[4]; buf[7] = time_f[5]; buf[8] = '\0';
            }
            memcpy((void*)s_gps_data.utc_time, buf, sizeof(buf));
        }

        if (date_f && date_f[0]) {
            char buf[11] = {0};
            int len = strlen(date_f);
            if (len >= 6) {
                buf[0] = date_f[0]; buf[1] = date_f[1]; buf[2] = '/';
                buf[3] = date_f[2]; buf[4] = date_f[3]; buf[5] = '/';
                buf[6] = date_f[4]; buf[7] = date_f[5]; buf[8] = '\0';
            }
            memcpy((void*)s_gps_data.utc_date, buf, sizeof(buf));
        }

        if (lat_f && lat_dir && lon_f && lon_dir) {
            s_gps_data.latitude  = nmea_to_decimal(lat_f, lat_dir[0]);
            s_gps_data.longitude = nmea_to_decimal(lon_f, lon_dir[0]);
        }

        if (spd_f && spd_f[0]) {
            s_gps_data.speed_knots = atof(spd_f);
            s_gps_data.speed_kmh   = s_gps_data.speed_knots * 1.852f;
        }

        s_gps_data.valid = true;
    }
    // ---- 解析 $GPGGA（定位数据，含卫星数和海拔） ----
    else if (strncmp(sentence + 1, "GPGGA", 5) == 0 ||
             strncmp(sentence + 1, "GNGGA", 5) == 0) {

        const char *sat_f  = nmea_field(sentence, 7);
        const char *alt_f  = nmea_field(sentence, 9);

        if (sat_f && sat_f[0]) {
            s_gps_data.satellites = atoi(sat_f);
        }
        if (alt_f && alt_f[0]) {
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

    // NMEA 语句累积缓冲区（一条语句可能跨多次读取）
    static char sentence[256];
    static int  sentence_len = 0;

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, buf, GPS_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        // 逐字符解析，提取完整的 NMEA 语句
        for (int i = 0; i < len; i++) {
            char c = (char)buf[i];

            if (c == '$') {
                // 新语句开始
                sentence_len = 0;
                sentence[sentence_len++] = c;
            }
            else if (sentence_len > 0 && sentence_len < (int)sizeof(sentence) - 1) {
                sentence[sentence_len++] = c;

                // 遇到换行符 = 一条完整语句
                if (c == '\n' || c == '\r') {
                    sentence[sentence_len] = '\0';
                    parse_nmea(sentence);
                    sentence_len = 0;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(buf);
}

// ============================================================
//   初始化 GPS 模块
// ============================================================
esp_err_t gps_init(void) {
    if (is_initialized) {
        ESP_LOGW(TAG, "GPS 已初始化，跳过");
        return ESP_OK;
    }

    // UART 配置
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

    // 启动后台解析任务
    xTaskCreatePinnedToCore(
        gps_read_task, "gps_task", GPS_TASK_STACK,
        NULL, GPS_TASK_PRIO, &gps_task_handle, 0
    );

    is_initialized = true;
    return ESP_OK;
}

// ============================================================
//   获取最新 GPS 数据
// ============================================================
gps_data_t gps_get_data(void) {
    return (gps_data_t)s_gps_data;
}

// ============================================================
//   释放 GPS 资源
// ============================================================
esp_err_t gps_deinit(void) {
    if (!is_initialized) return ESP_OK;

    if (gps_task_handle) {
        vTaskDelete(gps_task_handle);
        gps_task_handle = NULL;
    }

    uart_driver_delete(GPS_UART_NUM);
    is_initialized = false;
    memset((void*)&s_gps_data, 0, sizeof(s_gps_data));

    ESP_LOGI(TAG, "GPS 已释放");
    return ESP_OK;
}
