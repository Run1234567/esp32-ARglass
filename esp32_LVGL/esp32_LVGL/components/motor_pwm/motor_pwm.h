#ifndef _MOTOR_PWM_H
#define _MOTOR_PWM_H

#include <stdint.h>

// 初始化震动马达 PWM
void motor_pwm_init(void);

// 设置震动强度 (0-100%)
void motor_set_vibration(uint8_t percentage);

// 短促震动（模拟按键反馈）
void motor_pulse_short(void);

#endif
