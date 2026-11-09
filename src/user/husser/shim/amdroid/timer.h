#ifndef AMDROID_TIMER_H
/* User-side timer: the same monotonic ticks the kernel counts, read with
 * SYS_TICKS (int 0x80 with rax = 5), 100 Hz. */
#ifndef AMDROID_TIMER_H
#define AMDROID_TIMER_H
#include "amdroid/types.h"
static inline u64 timer_ticks(void){
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(5) : "memory");
    return (u64)r;
}
#endif
#endif
