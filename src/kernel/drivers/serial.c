#include "amdroid/serial.h"
#include "amdroid/types.h"

#define COM1 0x3F8

static inline void outb(u16 port, u8 val){ __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline u8 inb(u16 port){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

void serial_init(void){
    outb(COM1+1, 0x00); // disable interrupts
    outb(COM1+3, 0x80); // DLAB on
    outb(COM1+0, 0x03); // divisor low 38400
    outb(COM1+1, 0x00); // divisor high
    outb(COM1+3, 0x03); // 8 bits, no parity, 1 stop
    outb(COM1+2, 0xC7); // FIFO enable
    outb(COM1+4, 0x0B); // IRQs enabled, RTS/DSR set
}

static int serial_tx_ready(void){ return inb(COM1+5) & 0x20; }
static int serial_rx_ready(void){ return inb(COM1+5) & 0x01; }

void serial_putc(char c){
    while(!serial_tx_ready());
    outb(COM1, c);
    if(c=='\n'){ while(!serial_tx_ready()); outb(COM1,'\r'); }
}

void serial_puts(const char *s){ while(*s) serial_putc(*s++); }

int serial_has_input(void){ return serial_rx_ready(); }
int serial_getc(void){ if(!serial_rx_ready()) return -1; return inb(COM1); }
