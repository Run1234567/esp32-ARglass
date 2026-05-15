#ifndef MY_BLE_H
#define MY_BLE_H

#include <stdint.h>
#include <stdbool.h>

// 初始化蓝牙客户端 (主板)
void my_ble_init(const char* device_name);

// 向板子A (Server) 发送字符串数据
// 返回 true 表示发送成功，false 表示未连接或发送失败
bool my_ble_send_data(const char* data);

#endif // MY_BLE_H