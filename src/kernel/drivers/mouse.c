#include "amdroid/mouse.h"
#include "amdroid/types.h"

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline u8 inb(u16 p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}

/* Packet parser state and what has piled up since the last mouse_poll. mouse_feed() fills
   these from either mouse_poll() or the keyboard IRQ (which sees mouse bytes too). */
static int mouse_cycle = 0;
static signed char mouse_bytes[4];
static int packet_len = 3;             /* 4 when the mouse has a wheel */
static int acc_x = 0, acc_y = 0, acc_wheel = 0, acc_btns = 0, acc_pressed = 0, acc_got = 0;

static void mouse_wait(u8 type){
    int timeout=100000;
    if(type==0){ while(timeout--){ if((inb(0x64)&1)==1) return; } }
    else { while(timeout--){ if((inb(0x64)&2)==0) return; } }
}

static u8 mouse_write(u8 val){
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, val);
    mouse_wait(0);
    return inb(0x60); /* ack */
}

static void set_rate(u8 hz){ mouse_write(0xF3); mouse_write(hz); }

void mouse_init(void){
    /* keep IRQ1 out while we talk to the controller, otherwise its command
       replies get read as keyboard scancodes */
    __asm__ volatile("cli");
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
    /* the wheel: a mouse that has one answers id 3 after this sequence of sample rates,
       and then sends a fourth byte with the wheel movement */
    set_rate(200); set_rate(100); set_rate(80);
    mouse_write(0xF2);
    mouse_wait(0);
    packet_len = inb(0x60) == 3 ? 4 : 3;
    mouse_cycle = 0;
    acc_x = acc_y = acc_wheel = acc_pressed = acc_got = 0;
    mouse_write(0xF4); /* enable reporting */
    __asm__ volatile("sti");
}

/* Turn off packet streaming and discard leftover mouse bytes. Without this,
   the mouse keeps filling the controller's output buffer after the GUI exits,
   which stalls keyboard input in the text shell. */
void mouse_disable(void){
    __asm__ volatile("cli");
    mouse_write(0xF5);
    for(int n = 0; n < 64; n++){
        u8 st = inb(0x64);
        if(!(st & 1) || !(st & 0x20)) break;
        inb(0x60);
    }
    mouse_cycle = 0;
    __asm__ volatile("sti");
}

static void buttons(int btns){
    acc_pressed |= btns & ~acc_btns;      /* a press is kept even if it is let go before the poll */
    acc_btns = btns;
}

/* Feed one byte from the aux port into the packet parser. */
void mouse_feed(u8 raw){
    signed char b = (signed char)raw;
    if(mouse_cycle==0 && !(b & 0x08)) return;   /* resync on the always-set bit */
    mouse_bytes[mouse_cycle++] = b;
    if(mouse_cycle==packet_len){
        mouse_cycle = 0;
        buttons(mouse_bytes[0] & 0x07);
        int x = (unsigned char)mouse_bytes[1];
        int y = (unsigned char)mouse_bytes[2];
        if(mouse_bytes[0]&0x10) x -= 256;
        if(mouse_bytes[0]&0x20) y -= 256;
        /* PS/2 y is up-positive; screen y is down-positive */
        acc_x += x;
        acc_y += -y;
        if(packet_len == 4){
            int z = mouse_bytes[3] & 0x0F;      /* 4-bit signed: positive is towards the user */
            if(z & 8) z -= 16;
            acc_wheel += z;
        }
        acc_got = 1;
    }
}

/* Motion from another mouse (USB): dx right-positive, dy down-positive. */
void mouse_inject(int dx, int dy, int btns){
    acc_x += dx;
    acc_y += dy;
    buttons(btns);
    acc_got = 1;
}

/* Drain every pending mouse packet in one call, so the cursor does not lag
   one packet per frame. Only bytes flagged as aux (status bit 5) are read;
   keyboard bytes are left in the buffer for the keyboard driver.
   ev->pressed has a bit for each button that went down since the last call. */
int mouse_poll(mouse_event_t *ev){
    for(int n = 0; n < 256; n++){
        u8 st = inb(0x64);
        if(!(st & 1)) break;          /* output buffer empty */
        if(!(st & 0x20)) break;       /* next byte is keyboard, leave it */
        mouse_feed(inb(0x60));
    }
    if(!acc_got) return 0;
    ev->dx = acc_x; ev->dy = acc_y; ev->wheel = acc_wheel;
    ev->btns = acc_btns; ev->pressed = acc_pressed;
    acc_x = acc_y = acc_wheel = acc_pressed = acc_got = 0;
    return 1;
}
