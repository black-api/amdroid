#ifndef AMDROID_EXT2_H
#define AMDROID_EXT2_H
#include "amdroid/types.h"

/* Read-only ext2 on the secondary IDE disk (see ext2.c). */
int  ext2_mount(void);                                   /* 0 if a valid ext2 file system is found */
typedef void (*ext2_list_fn)(const char *name, int is_dir, u32 size);
int  ext2_list(const char *path, ext2_list_fn cb);       /* "" or "/" for the root */
int  ext2_read(const char *path, void *buf, u32 max, u32 *size);
int  ext2_mounted(void);
#endif
