#ifndef MY_UART_H
#define MY_UART_H

#include "esp_err.h"

#define UART_NUM UART_NUM_1
#define TXD_PIN 4
#define RXD_PIN 5
#define BUF_SIZE 1024

void my_uart_init(void);
void my_uart_send(const char* data);

#endif // MY_UART_H
