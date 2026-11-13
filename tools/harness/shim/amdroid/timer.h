#ifndef AMDROID_TIMER_H
#define AMDROID_TIMER_H
#include "amdroid/types.h"
/* Host build of the web engine: a monotonic tick that advances a little on
 * every read, so any animation/time logic makes progress without an OS. */
static inline u64 timer_ticks(void){
    static u64 t = 0;
    return t += 1;
}
#endif
