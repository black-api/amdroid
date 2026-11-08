/* pci.c — PCI configuration access (mechanism 1, ports 0xCF8/0xCFC) and
 * device enumeration. */
#include "amdroid/pci.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

#define MAX_DEVS 64
static pci_dev_t devs[MAX_DEVS];
static int ndevs;

static inline void outl(u16 p, u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u32 inl(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off){
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)(dev & 31) << 11) | ((u32)(fn & 7) << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_cfg_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val){
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)(dev & 31) << 11) | ((u32)(fn & 7) << 8) | (off & 0xFC));
    outl(0xCFC, val);
}

void pci_init(void){
    ndevs = 0;
    for(int bus = 0; bus < 256; bus++){
        for(int dev = 0; dev < 32; dev++){
            u32 id = pci_cfg_read32((u8)bus, (u8)dev, 0, 0);
            if((id & 0xFFFF) == 0xFFFF) continue;
            int multi = (pci_cfg_read32((u8)bus, (u8)dev, 0, 0x0C) >> 16) & 0x80;
            for(int fn = 0; fn < (multi ? 8 : 1); fn++){
                u32 idf = pci_cfg_read32((u8)bus, (u8)dev, (u8)fn, 0);
                if((idf & 0xFFFF) == 0xFFFF) continue;
                if(ndevs == MAX_DEVS) return;
                pci_dev_t *d = &devs[ndevs++];
                d->bus = (u8)bus; d->dev = (u8)dev; d->fn = (u8)fn;
                d->vendor = (u16)(idf & 0xFFFF);
                d->device = (u16)(idf >> 16);
                u32 cls = pci_cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x08);
                d->class_code = (u8)(cls >> 24);
                d->subclass = (u8)(cls >> 16);
                d->prog_if = (u8)(cls >> 8);
                u32 l = pci_cfg_read32((u8)bus, (u8)dev, (u8)fn, 0x3C);
                d->irq_line = (u8)l;
                for(int b = 0; b < 6; b++) d->bar[b] = pci_cfg_read32((u8)bus, (u8)dev, (u8)fn, (u8)(0x10 + 4*b));
            }
        }
    }
    console_printf("[pci] %d devices\n", ndevs);
}

int pci_count(void){ return ndevs; }
pci_dev_t *pci_get(int i){ return (i >= 0 && i < ndevs) ? &devs[i] : 0; }

pci_dev_t *pci_find(u16 vendor, u16 device){
    for(int i = 0; i < ndevs; i++){
        if((vendor == 0xFFFF || devs[i].vendor == vendor) && (device == 0xFFFF || devs[i].device == device))
            return &devs[i];
    }
    return 0;
}

pci_dev_t *pci_find_class(u8 cls, u8 subcls){
    for(int i = 0; i < ndevs; i++)
        if(devs[i].class_code == cls && (subcls == 0xFF || devs[i].subclass == subcls)) return &devs[i];
    return 0;
}

void pci_enable(pci_dev_t *d){
    u32 cmd = pci_cfg_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x7;      /* I/O space, memory space, bus master */
    pci_cfg_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

u64 pci_bar_base(pci_dev_t *d, int bar){
    u32 lo = d->bar[bar];
    if(lo & 1) return 0;                      /* I/O BAR */
    u64 base = lo & ~0xFULL;
    if(((lo >> 1) & 3) == 2 && bar < 5)       /* 64-bit BAR */
        base |= (u64)d->bar[bar + 1] << 32;
    return base;
}
