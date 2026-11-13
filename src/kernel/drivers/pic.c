#include "amdroid/pic.h"
#include "amdroid/types.h"

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}

void pic_init(void){
    outb(0x20,0x11); outb(0xA0,0x11);
    outb(0x21,0x20); outb(0xA1,0x28);
    outb(0x21,0x04); outb(0xA1,0x02);
    outb(0x21,0x01); outb(0xA1,0x01);
    /* mask all except timer(0) and keyboard(1) */
    outb(0x21,0xFC); outb(0xA1,0xFF);
}

