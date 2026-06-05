/**
 * @file my_uart.h
 * @brief UART 命令接口模块公共接口
 *
 * 本模块通过 UART1 (1Mbaud) 与外部 UI MCU 通信。
 * GPIO 4 (TX), GPIO 5 (RX)
 */

#ifndef MY_UART_H
#define MY_UART_H

#include "esp_err.h"

/* =====================================================================
 * UART 硬件配置
 * ===================================================================== */
#define UART_NUM  UART_NUM_1   // 使用 UART1 (UART0 通常用于日志/下载)
#define TXD_PIN   4            // 发送引脚: GPIO 4
#define RXD_PIN   5            // 接收引脚: GPIO 5
#define BUF_SIZE  4096         // UART 缓冲区大小: 4KB

/**
 * @brief 初始化 UART1 驱动
 *
 * 配置 1Mbaud/8N1，启动事件处理任务 (优先级 12)。
 * 必须在 app_main() 中调用一次。
 */
void my_uart_init(void);

/**
 * @brief 通过 UART1 发送字符串
 *
 * @param data 要发送的以 '\0' 结尾的字符串
 *
 * 此函数是阻塞式的，会等待数据写入 UART FIFO。
 * 在高频调用场景下，建议在调用间加入适当延时。
 */
void my_uart_send(const char* data);

#endif // MY_UART_H
