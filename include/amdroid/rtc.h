#ifndef AMDROID_RTC_H
#define AMDROID_RTC_H
#include "amdroid/types.h"
typedef struct { u16 year; u8 month, day, hour, min, sec; } rtc_time_t;
void rtc_read(rtc_time_t *t);   /* the CMOS real-time clock (UTC as the BIOS keeps it) */
#endif
