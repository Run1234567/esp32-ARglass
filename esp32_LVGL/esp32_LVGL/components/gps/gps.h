// ============================================================
// gps.h
// J.A.R.V.I.S. AR 智能眼镜 —— GPS 模块头文件
// ============================================================
// 硬件：ATGM336H 北斗+GPS 双模定位模块
// 接线：VCC → 3.3V，GND → GND，TX → GPIO 18，RX → GPIO 17
// 波特率：9600（默认）
// ============================================================

#ifndef _GPS_H
#define _GPS_H

#include "esp_err.h"
#include <stdbool.h>

// GPS 数据结构体
typedef struct {
    double latitude;        // 纬度（十进制度）
    double longitude;       // 经度（十进制度）
    float  speed_knots;     // 速度（节）
    float  speed_kmh;       // 速度（千米/时）
    float  altitude;        // 海拔高度（米）
    int    satellites;      // 可见卫星数
    char   utc_time[11];    // UTC 时间 "HH:MM:SS"
    char   utc_date[11];    // UTC 日期 "DD/MM/YY"
    bool   valid;           // 定位是否有效（true = 已定位）
} gps_data_t;

// 初始化 GPS 模块（UART 配置 + 后台解析任务启动）
esp_err_t gps_init(void);

// 获取最新的 GPS 数据（线程安全，由后台任务持续更新）
gps_data_t gps_get_data(void);

// 释放 GPS 资源
esp_err_t gps_deinit(void);

#endif // _GPS_H
