/* heap_shim.c — a bump allocator over a static arena, for the Husser
 * renderer. Husser owns exactly one page and exits when the browser
 * navigates, so blocks never need to be reused; this keeps the port small
 * and free of fragmentation bugs. kfree is a no-op, krealloc copies. */
#include "amdroid/types.h"
#include "string.h"

#define ARENA_BYTES (32 * 1024 * 1024)
static u8 arena[ARENA_BYTES];
static u64 arena_off = 0;

static void *bump(size_t n){
    u64 off = (arena_off + 15) & ~15ULL;
    if(off + n > ARENA_BYTES) return NULL;
    arena_off = off + n;
    return arena + off;
}

void *kmalloc(size_t size){ return bump(size ? size : 1); }
void  kfree(void *ptr){ (void)ptr; }                       /* one page per process */
void *kcalloc(size_t n, size_t sz){
    size_t t = n * sz;
    void *p = bump(t);
    if(p) memset(p, 0, t);
    return p;
}
void *krealloc(void *ptr, size_t size){
    (void)ptr;
    return bump(size);                                      /* old block stays dead */
}
