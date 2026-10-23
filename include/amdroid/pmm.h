#ifndef AMDROID_PMM_H
#define AMDROID_PMM_H
#include "amdroid/types.h"

void pmm_init(u32 mbi);
void *pmm_alloc_page(void);
void pmm_free_page(void *p);
u64 pmm_total_pages(void);
u64 pmm_free_pages(void);

#endif
