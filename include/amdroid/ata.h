#ifndef AMDROID_ATA_H
#define AMDROID_ATA_H
#include "amdroid/types.h"
int ata_init(void);
int ata_read_sectors(u32 lba, u8 count, void *buf);
int ata_write_sectors(u32 lba, u8 count, const void *buf);
int ata_dev_present(int chan, int slave);                 /* chan 0 = primary IDE, 1 = secondary */
int ata_dev_read(int chan, int slave, u32 lba, u8 count, void *buf);
int ata_dev_write(int chan, int slave, u32 lba, u8 count, const void *buf);
#endif
