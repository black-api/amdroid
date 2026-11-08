#ifndef AMDROID_USB_H
#define AMDROID_USB_H
#include "amdroid/types.h"
/* A USB mass-storage disk (bulk-only transport, SCSI commands). */
int usb_disk_present(void);
void usb_disk_info(u32 *blocks, u32 *block_size, char vendor[9], char product[17]);
int usb_disk_read(u32 lba, u32 count, void *buf);
int usb_disk_write(u32 lba, u32 count, const void *buf);
#endif
