#include "amdroid/timer.h"
#include "amdroid/types.h"

static volatile u64 ticks = 0;
static u32 timer_hz = 100;

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}

void timer_init(u32 hz){
    timer_hz = hz;
    u32 div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, (div>>8)&0xFF);
}

u64 timer_ticks(void){ return ticks; }

/* called from isr handler for vector 32 */
void timer_tick(void){ ticks++; }

void timer_sleep(u64 ms){
    u64 target = ticks + (ms * timer_hz / 1000) + 1;
    while(ticks < target) __asm__ volatile("hlt");
}
