#include "amdroid/mouse.h"
#include "amdroid/types.h"

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline u8 inb(u16 p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}

static int mouse_cycle = 0;
static signed char mouse_bytes[3];
static int mx=0, my=0, mbtns=0;
static int has_new = 0;

static void mouse_wait(u8 type){
    int timeout=100000;
    if(type==0){ while(timeout--){ if((inb(0x64)&1)==1) return; } }
    else { while(timeout--){ if((inb(0x64)&2)==0) return; } }
}

static void mouse_write(u8 val){
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
    mouse_wait(0);
    inb(0x60); /* ack */
}

void mouse_init(void){
    /* enable aux */
    mouse_wait(1);
    outb(0x64, 0xA8);
    /* enable interrupts (IRQ12) */
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    u8 st = inb(0x60);
    st |= 2;
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, st);
    /* set defaults */
    mouse_write(0xF6);
    mouse_write(0xF4); /* enable reporting */
}

/* Drain every pending mouse packet in one call, so the cursor does not lag
   one packet per frame. Only bytes flagged as aux (status bit 5) are read;
   keyboard bytes are left in the buffer for the keyboard driver. */
int mouse_poll(int *dx, int *dy, int *btns){
    int sum_x = 0, sum_y = 0, got = 0;
    for(int n = 0; n < 256; n++){
        u8 st = inb(0x64);
        if(!(st & 1)) break;          /* output buffer empty */
        if(!(st & 0x20)) break;       /* next byte is keyboard, leave it */
        signed char b = (signed char)inb(0x60);
        if(mouse_cycle==0 && !(b & 0x08)) continue; /* resync */
        mouse_bytes[mouse_cycle++] = b;
        if(mouse_cycle==3){
            mouse_cycle = 0;
            mbtns = mouse_bytes[0] & 0x07;
            int x = (unsigned char)mouse_bytes[1];
            int y = (unsigned char)mouse_bytes[2];
            if(mouse_bytes[0]&0x10) x -= 256;
            if(mouse_bytes[0]&0x20) y -= 256;
            /* PS/2 y is up-positive; screen y is down-positive */
            sum_x += x;
            sum_y += -y;
            got = 1;
        }
    }
    if(!got) return 0;
    mx += sum_x;
    my += sum_y;
    if(mx<0) mx=0;
    if(my<0) my=0;
    if(dx) *dx = sum_x;
    if(dy) *dy = sum_y;
    if(btns) *btns = mbtns;
    has_new = 1;
    return 1;
}
