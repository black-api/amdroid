#ifndef AMDROID_KERNEL_H
#define AMDROID_KERNEL_H

#include "amdroid/types.h"

/* kernel main entry (called from boot.asm) */
void kernel_main(u32 magic, u32 mbi);

#endif
