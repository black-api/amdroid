/* ata.c — ATA PIO disk driver for the two IDE channels (master drives).
 * Channel 0 (ports 0x1F0) is the boot disk; channel 1 (0x170) is optional. */
#include "amdroid/ata.h"
#include "amdroid/types.h"
#include "amdroid/console.h"

static inline void outb(u16 p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline u8 inb(u16 p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static inline u16 inw(u16 p){u16 v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p));return v;}
static inline void outw(u16 p,u16 v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}

static const u16 base_of[2] = { 0x1F0, 0x170 };

static void ata_wait(u16 b){
    for(int i=0;i<100000;i++){
        u8 s = inb(b + 7);
        if(!(s & 0x80)) return;
    }
}

int ata_present(int chan){
    u16 b = base_of[chan];
    outb(b + 6, 0xE0);
    ata_wait(b);
    u8 s = inb(b + 7);
    return !(s == 0x00 || s == 0xFF);
}

int ata_init(void){
    if(!ata_present(0)){ console_puts("[ata] no disk\n"); return -1; }
    console_puts("[ata] disk present\n");
    return 0;
}

static int ata_read_at(u16 b, u32 lba, u8 count, void *buf, u8 drv){
    ata_wait(b);
    outb(b + 6, drv | ((lba>>24)&0x0F));
    outb(b + 2, count);
    outb(b + 3, lba & 0xFF);
    outb(b + 4, (lba>>8)&0xFF);
    outb(b + 5, (lba>>16)&0xFF);
    outb(b + 7, 0x20); /* read */
    u16 *dst = buf;
    for(int s=0;s<count;s++){
        int ok = 0;
        for(int t=0;t<1000000;t++){ u8 st=inb(b + 7); if(st&0x08){ ok=1; break; } if(st&0x01) return -1; }
        if(!ok) return -1;
        for(int i=0;i<256;i++) dst[s*256+i] = inw(b);
        ata_wait(b);
    }
    return 0;
}

static int ata_write_at(u16 b, u32 lba, u8 count, const void *buf, u8 drv){
    ata_wait(b);
    outb(b + 6, drv | ((lba>>24)&0x0F));
    outb(b + 2, count);
    outb(b + 3, lba & 0xFF);
    outb(b + 4, (lba>>8)&0xFF);
    outb(b + 5, (lba>>16)&0xFF);
    outb(b + 7, 0x30); /* write */
    const u16 *src = buf;
    for(int s=0;s<count;s++){
        int ok = 0;
        for(int t=0;t<1000000;t++){ u8 st=inb(b + 7); if(st&0x08){ ok=1; break; } if(st&0x01) return -1; }
        if(!ok) return -1;
        for(int i=0;i<256;i++) outw(b, src[s*256+i]);
        ata_wait(b);
    }
    outb(b + 7, 0xE7);     /* flush */
    ata_wait(b);
    return 0;
}

int ata_read_sectors(u32 lba, u8 count, void *buf){ return ata_read_at(0x1F0, lba, count, buf, 0xE0); }
int ata_write_sectors(u32 lba, u8 count, const void *buf){ return ata_write_at(0x1F0, lba, count, buf, 0xE0); }

/* the second channel, for a second disk */
/* slave: 0 = master drive, 1 = slave drive on the same channel */
int ata_dev_present(int chan, int slave){
    u16 b = base_of[chan];
    outb(b + 6, slave ? 0xF0 : 0xE0);
    ata_wait(b);
    u8 s = inb(b + 7);
    return !(s == 0x00 || s == 0xFF);
}
int ata_dev_read(int chan, int slave, u32 lba, u8 count, void *buf){
    return ata_read_at(base_of[chan], lba, count, buf, slave ? 0xF0 : 0xE0);
}
int ata_dev_write(int chan, int slave, u32 lba, u8 count, const void *buf){
    return ata_write_at(base_of[chan], lba, count, buf, slave ? 0xF0 : 0xE0);
}
