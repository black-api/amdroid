#ifndef AMDROID_ATA_H
#define AMDROID_ATA_H
#include "amdroid/types.h"
int ata_init(void);
int ata_read_sectors(u32 lba, u8 count, void *buf);
int ata_write_sectors(u32 lba, u8 count, const void *buf);
#endif
