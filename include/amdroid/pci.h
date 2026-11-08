#ifndef AMDROID_PCI_H
#define AMDROID_PCI_H
#include "amdroid/types.h"

typedef struct {
    u8 bus, dev, fn;
    u16 vendor, device;
    u8 class_code, subclass, prog_if, irq_line;
    u32 bar[6];          /* as read from config space (memory BARs have the low bits set) */
} pci_dev_t;

void pci_init(void);                              /* scans the bus once */
int  pci_count(void);
pci_dev_t *pci_get(int i);
pci_dev_t *pci_find(u16 vendor, u16 device);      /* 0xFFFF matches anything */
pci_dev_t *pci_find_class(u8 cls, u8 subcls);
u32  pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off);
void pci_cfg_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val);
void pci_enable(pci_dev_t *d);                    /* bus master + memory + I/O space */
u64  pci_bar_base(pci_dev_t *d, int bar);         /* physical base of a memory BAR (64-bit aware) */
#endif
