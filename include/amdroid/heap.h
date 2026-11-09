#ifndef AMDROID_HEAP_H
#define AMDROID_HEAP_H
#include "amdroid/types.h"

void  heap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t n, size_t sz);
void *krealloc(void *ptr, size_t size);
extern int heap_owner;                       /* tag for new blocks (0 = kernel, n+1 = GUI app n) */
void  heap_stats(u64 *used, u64 *total, u64 *by_owner, int nowners);

#endif
