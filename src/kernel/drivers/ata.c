#include "amdroid/ata.h"
#include "amdroid/types.h"
#include "amdroid/console.h"

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline u8 inb(u16 p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static inline void outw(u16 p,u16 v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline u16 inw(u16 p){u16 v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p));return v;}

static void ata_wait(void){
    for(int i=0;i<100000;i++){
        u8 s = inb(0x1F7);
        if(!(s & 0x80)) return;
    }
}

int ata_init(void){
    outb(0x1F6, 0xE0);
    ata_wait();
    u8 s = inb(0x1F7);
    if(s==0x00 || s==0xFF){ console_puts("[ata] no disk\n"); return -1; }
    console_puts("[ata] disk present\n");
    return 0;
}

int ata_read_sectors(u32 lba, u8 count, void *buf){
    ata_wait();
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F2, count);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba>>8)&0xFF);
    outb(0x1F5, (lba>>16)&0xFF);
    outb(0x1F7, 0x20); /* read */
    u16 *dst = buf;
    for(int s=0;s<count;s++){
        /* wait for DRQ */
        for(int t=0;t<1000000;t++){ u8 st=inb(0x1F7); if(st&0x08) break; if(st&0x01){ return -1; } if(t==999999) return -1; }
        for(int i=0;i<256;i++) dst[s*256+i] = inw(0x1F0);
        /* wait not busy */
        ata_wait();
    }
    return 0;
}

int ata_write_sectors(u32 lba, u8 count, const void *buf){
    ata_wait();
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F2, count);
    outb(0x1F3, lba & 0xFF);
    outb(0x1F4, (lba>>8)&0xFF);
    outb(0x1F5, (lba>>16)&0xFF);
    outb(0x1F7, 0x30); /* write */
    const u16 *src = buf;
    for(int s=0;s<count;s++){
        for(;;){ u8 st=inb(0x1F7); if(st&0x08) break; }
        for(int i=0;i<256;i++) outw(0x1F0, src[s*256+i]);
        ata_wait();
    }
    /* flush */
    outb(0x1F7, 0xE7);
    ata_wait();
    return 0;
}
