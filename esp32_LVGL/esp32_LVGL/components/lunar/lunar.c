#include "lunar.h"
#include <stdio.h>
#include <string.h>

// 农历数据表 (2000-2050，正确的数据)
static const unsigned int lunar_info[] = {
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04bd7, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63
};

static const char *TianGan[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
static const char *DiZhi[]   = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
static const char *LunarMonth[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
static const char *DayPrefix[]  = {"初", "十", "廿", "三十"};
static const char *DaySuffix[]  = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九"};

static int days_since_2000(int y, int m, int d) {
    int days = 0;
    int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int i = 2000; i < y; i++) {
        days += 365 + ((i % 4 == 0 && i % 100 != 0) || (i % 400 == 0));
    }
    if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) days_in_month[2] = 29;
    for (int i = 1; i < m; i++) days += days_in_month[i];
    return days + d - 1;
}

void get_lunar_string(int year, int month, int day, char *out_str) {
    if (year < 2000 || year > 2050) {
        sprintf(out_str, "N/A");
        return;
    }

    int offset = days_since_2000(year, month, day) - 35;

    if (offset < 0) {
        sprintf(out_str, "N/A");
        return;
    }

    int i, leap_month = 0, temp = 0;
    int lunar_year = 2000;

    for (i = 2000; i <= 2050 && offset >= 0; i++) {
        temp = 0;
        for (int m = 0x8000; m > 0x8; m >>= 1) {
            temp += (lunar_info[i - 2000] & m) ? 30 : 29;
        }
        leap_month = lunar_info[i - 2000] & 0xf;
        if (leap_month) {
            temp += (lunar_info[i - 2000] & 0x10000) ? 30 : 29;
        }
        offset -= temp;
        lunar_year++;
    }

    if (offset < 0) {
        offset += temp;
        lunar_year--;
    }

    leap_month = lunar_info[lunar_year - 2000] & 0xf;
    int is_leap = 0;
    int lunar_month = 1;

    for (i = 1; i <= 12 && offset >= 0; i++) {
        if (leap_month > 0 && i == (leap_month + 1) && !is_leap) {
            --i;
            is_leap = 1;
            temp = (lunar_info[lunar_year - 2000] & 0x10000) ? 30 : 29;
        } else {
            temp = (lunar_info[lunar_year - 2000] & (0x10000 >> i)) ? 30 : 29;
        }
        offset -= temp;
        if (is_leap && i == leap_month) is_leap = 0;
        lunar_month++;
    }

    if (offset < 0) {
        offset += temp;
        lunar_month--;
    }

    int lunar_day = offset + 1;
    int tg = (lunar_year - 4) % 10;
    int dz = (lunar_year - 4) % 12;

    char day_str[10];
    if (lunar_day == 10) sprintf(day_str, "初十");
    else if (lunar_day == 20) sprintf(day_str, "二十");
    else if (lunar_day == 30) sprintf(day_str, "三十");
    else sprintf(day_str, "%s%s", DayPrefix[lunar_day / 10], DaySuffix[lunar_day % 10]);

    sprintf(out_str, "%s%s %s%s月%s",
            TianGan[tg], DiZhi[dz],
            is_leap ? "闰" : "", LunarMonth[lunar_month - 1], day_str);
}
