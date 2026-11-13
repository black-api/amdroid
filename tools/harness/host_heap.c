/* host_heap.c — kmalloc & co. over libc, for host test builds (no size limit). */
#include <stdlib.h>
#include <string.h>
void *kmalloc(size_t n){ return malloc(n ? n : 1); }
void  kfree(void *p){ free(p); }
void *kcalloc(size_t n, size_t sz){ return calloc(n, sz); }
void *krealloc(void *p, size_t n){ return realloc(p, n); }
