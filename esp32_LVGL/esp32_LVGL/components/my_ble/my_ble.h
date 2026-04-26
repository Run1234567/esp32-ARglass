#ifndef MY_BLE_H
#define MY_BLE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化低功耗蓝牙 (NimBLE)
 * * @param device_name 显示在手机上的蓝牙名称
 */
void my_ble_init(const char* device_name);

#ifdef __cplusplus
}
#endif

#endif // MY_BLE_H