#ifndef MY_WIFI_H
#define MY_WIFI_H

// ��¶���ⲿ���õ� Wi-Fi ��ʼ������
void wifi_init_sta(void);
void my_wifi_connect_from_ble(const char* ssid, const char* password);
void save_wifi_to_nvs(const char* ssid, const char* pwd);

// WiFi 历史记录结构体（供外部读取用）
typedef struct {
    char ssid[33];
    char password[65];
} wifi_record_t;

int load_wifi_history(wifi_record_t *out_records, int max_count);

#endif // MY_WIFI_H