#ifndef AMDROID_FAT32_H
#define AMDROID_FAT32_H
#include "amdroid/types.h"
int fat32_init(void);                 /* returns 0 on success */
int fat32_list_root(void (*cb)(const char *name, int is_dir, u32 size));
int fat32_read_file(const char *name, void *buf, u32 max, u32 *out_size);
int fat32_write_file(const char *name, const void *data, u32 size); /* 8.3 root file */
int fat32_delete_file(const char *name);
int fat32_read_path(const char *path, void *buf, u32 max, u32 *out_size); /* "DIR/NAME.EXT" */
int fat32_write_path(const char *path, const void *data, u32 size);   /* any directory */
int fat32_delete_path(const char *path);
int fat32_mkdir(const char *path);
int fat32_list_path(const char *path, void (*cb)(const char *name, int is_dir, u32 size));
int fat32_mount_usb(void);     /* use the FAT32 file system on the USB disk */
int fat32_mount_boot(void);    /* back to the boot disk */
int fat32_on_usb(void);
int fat32_recover(void);     /* frees clusters that no file uses (after a crash); count or -1 */
#endif
