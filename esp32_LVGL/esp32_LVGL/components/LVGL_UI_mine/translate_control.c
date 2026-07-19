/**
 * @file translate_control.c
 * @brief 翻译控制模块 —— 通过 UART 发送指令到翻译板
 */

#include "my_uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TRANSLATE_CTRL";
static bool translate_running = false;
static char server_ip[64] = "";
static int server_port = 5001;

/**
 * @brief 启动翻译服务
 * @param server_ip 服务器IP地址（NULL 使用默认）
 * @param port 服务器端口（0 使用默认 5001）
 */
void translate_start(const char *server_ip_addr, int port) {
    if (server_ip_addr != NULL && strlen(server_ip_addr) > 0) {
        strncpy(server_ip, server_ip_addr, sizeof(server_ip) - 1);
        server_ip[sizeof(server_ip) - 1] = '\0';
    }
    if (port > 0) {
        server_port = port;
    }

    // 发送启动指令到翻译板
    char cmd_buf[128];
    if (strlen(server_ip) > 0) {
        snprintf(cmd_buf, sizeof(cmd_buf), "CMD:TRANSLATE_START:%s:%d\r\n", server_ip, server_port);
    } else {
        snprintf(cmd_buf, sizeof(cmd_buf), "CMD:TRANSLATE_START\r\n");
    }
    my_uart_send(cmd_buf);

    translate_running = true;
    ESP_LOGI(TAG, "翻译启动: %s:%d", strlen(server_ip) > 0 ? server_ip : "默认", server_port);
}

/**
 * @brief 停止翻译服务
 */
void translate_stop(void) {
    my_uart_send("CMD:TRANSLATE_STOP\r\n");
    translate_running = false;
    ESP_LOGI(TAG, "翻译停止");
}

/**
 * @brief 获取翻译运行状态
 * @return true 运行中, false 已停止
 */
bool translate_is_running(void) {
    return translate_running;
}
