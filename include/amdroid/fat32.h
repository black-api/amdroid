#ifndef AMDROID_FAT32_H
#define AMDROID_FAT32_H
#include "amdroid/types.h"
int fat32_init(void);                 /* returns 0 on success */
int fat32_list_root(void (*cb)(const char *name, int is_dir, u32 size));
int fat32_read_file(const char *name, void *buf, u32 max, u32 *out_size);
#endif
