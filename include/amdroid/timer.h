#ifndef AMDROID_TIMER_H
#define AMDROID_TIMER_H
#include "amdroid/types.h"
void timer_init(u32 hz);
void timer_tick(void);
u64 timer_ticks(void);
void timer_sleep(u64 ms);
#endif
