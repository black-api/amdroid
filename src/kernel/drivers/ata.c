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
    for(long i=0;i<4000000;i++){
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

/* No drive selected: the status port reads all ones (floating) or all zeros (QEMU with no disk). Every read and write checks, so a
   machine with no disk fails at once instead of spending seconds per sector polling for one
   (opening the video player, which lists the disk, used to freeze the whole desktop). */
static int ata_floating(u16 b, u8 drv){
    outb(b + 6, drv);
    for(int i = 0; i < 4; i++) (void)inb(b + 7);       /* the 400 ns the drive select needs */
    u8 s = inb(b + 7);
    return s == 0x00 || s == 0xFF;
}

/* Waits until the drive is idle: not busy and not still asking for data. A
   command issued while either bit is set is aborted by the drive, which is
   what used to make a long write die halfway through. */
static void ata_idle(u16 b){
    for(long i=0;i<4000000;i++) if(!(inb(b + 7) & 0x80)) break;
    if(inb(b + 7) & 0x08)                       /* stale request: take it out */
        for(int i=0;i<256;i++) (void)inw(b);
}

/* The command block, with the 400 ns the spec asks for after drive/head. */
static void ata_setup(u16 b, u32 lba, u8 count, u8 drv){
    outb(b + 6, drv | ((lba>>24)&0x0F));
    for(int i=0;i<4;i++) (void)inb(b + 7);
    outb(b + 2, count);
    outb(b + 3, lba & 0xFF);
    outb(b + 4, (lba>>8)&0xFF);
    outb(b + 5, (lba>>16)&0xFF);
}

/* 0 when the drive wants its next sector, -1 if it reported an error. */
static int ata_drq(u16 b){
    for(long t=0;t<4000000;t++){
        u8 st = inb(b + 7);
        if(st & 0x01) return -1;
        if(st & 0x08) return 0;
    }
    return -1;
}

static int ata_read_at(u16 b, u32 lba, u8 count, void *buf, u8 drv){
    u16 *dst = buf;
    if(ata_floating(b, drv)) return -1;
    for(int try=0;try<3;try++){
        ata_idle(b);
        ata_setup(b, lba, count, drv);
        outb(b + 7, 0x20); /* read */
        int bad = 0;
        for(int s=0;s<count;s++){
            if(ata_drq(b) != 0){ bad = 1; break; }
            for(int i=0;i<256;i++) dst[s*256+i] = inw(b);
            ata_wait(b);
        }
        if(!bad) return 0;
    }
    console_printf("[ata] read of sector %u failed (status %x)\n", lba, (unsigned)inb(b + 7));
    return -1;
}

static int ata_write_at(u16 b, u32 lba, u8 count, const void *buf, u8 drv){
    const u16 *src = buf;
    if(ata_floating(b, drv)) return -1;
    for(int try=0;try<3;try++){
        ata_idle(b);
        ata_setup(b, lba, count, drv);
        outb(b + 7, 0x30); /* write */
        int bad = 0;
        for(int s=0;s<count;s++){
            if(ata_drq(b) != 0){ bad = 1; break; }
            for(int i=0;i<256;i++) outw(b, src[s*256+i]);
            ata_wait(b);
        }
        if(!bad) return 0;
    }
    console_printf("[ata] write of sector %u failed (status %x, error %x)\n",
                   lba, (unsigned)inb(b + 7), (unsigned)inb(b + 1));
    return -1;
}

int ata_read_sectors(u32 lba, u8 count, void *buf){ return ata_read_at(0x1F0, lba, count, buf, 0xE0); }
int ata_write_sectors(u32 lba, u8 count, const void *buf){ return ata_write_at(0x1F0, lba, count, buf, 0xE0); }

/* Puts everything written since the last call on the medium. The file system
   calls this at its own boundaries; flushing after every sector turned a few
   megabytes of video into a minute of waiting. */
int ata_flush(void){
    if(ata_floating(0x1F0, 0xE0)) return -1;
    ata_idle(0x1F0);
    outb(0x1F7, 0xE7);     /* flush cache */
    ata_wait(0x1F0);
    return 0;
}

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
