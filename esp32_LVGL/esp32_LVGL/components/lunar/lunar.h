#ifndef LUNAR_H
#define LUNAR_H

/**
 * @brief 将公历日期转换为农历字符串
 * @param year  公历年 (例如 2024)
 * @param month 公历月 (1-12)
 * @param day   公历日 (1-31)
 * @param out_str 输出缓冲区 (建议 32 字节)
 */
void get_lunar_string(int year, int month, int day, char *out_str);

#endif
