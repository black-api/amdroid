#ifndef AMDROID_VMM_H
#define AMDROID_VMM_H
#include "amdroid/types.h"
void vmm_map_region(u64 virt, u64 phys, u64 size);
#endif
