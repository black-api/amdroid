/* rtc.c — reads the CMOS real-time clock (ports 0x70/0x71). */
#include "amdroid/rtc.h"
#include "amdroid/types.h"

static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static u8 cmos(u8 reg){ outb(0x70, reg); return inb(0x71); }
static u8 from_bcd(u8 v){ return (u8)((v & 0x0F) + (v >> 4) * 10); }

void rtc_read(rtc_time_t *t){
    /* wait out an update in progress, then read twice until stable */
    rtc_time_t a, b;
    for(int tries = 0; tries < 2; tries++){
        while(cmos(0x0A) & 0x80) { }
        a.sec = cmos(0x00); a.min = cmos(0x02); a.hour = cmos(0x04);
        a.day = cmos(0x07); a.month = cmos(0x08); a.year = cmos(0x09);
        u8 century = cmos(0x32);
        u8 regb = cmos(0x0B);
        if(!(regb & 0x04)){            /* BCD mode: convert */
            a.sec = from_bcd(a.sec); a.min = from_bcd(a.min); a.hour = from_bcd(a.hour);
            a.day = from_bcd(a.day); a.month = from_bcd(a.month); a.year = from_bcd(a.year);
            century = from_bcd(century);
        }
        a.year = (u16)(century ? century * 100 + a.year : 2000 + a.year);
        b = a;
        if(tries == 1 || a.sec == b.sec) break;
    }
    *t = a;
}
