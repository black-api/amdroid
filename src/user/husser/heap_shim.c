/* heap_shim.c — the Husser renderer's allocator: first-fit over one region with real
 * free (blocks are merged with free neighbours), so big pages can load scripts, images
 * and stylesheets without running out. The kernel maps the region and tells Husser its
 * size (more in ultra mode); heap_init must run before the first allocation. */
#include "amdroid/types.h"
#include "string.h"

typedef struct blk {
    u64 size;                 /* bytes after the header */
    u64 free;
    struct blk *next_free;    /* free list, in address order */
} blk_t;
#define HDR ((u64)sizeof(blk_t))

static u8 *heap_base = NULL, *heap_end = NULL;
static blk_t *free_list = NULL;

void heap_init(void *base, u64 bytes){
    heap_base = base;
    heap_end = (u8 *)base + bytes;
    blk_t *b = (blk_t *)base;
    b->size = bytes - HDR;
    b->free = 1;
    b->next_free = NULL;
    free_list = b;
}

/* the block right after b in memory, or NULL at the end */
static blk_t *after(blk_t *b){
    u8 *n = (u8 *)b + HDR + b->size;
    return n < heap_end ? (blk_t *)n : NULL;
}

void *kmalloc(size_t size){
    if(!heap_base) return NULL;
    u64 n = (size + 15) & ~15ULL;
    if(n < 16) n = 16;
    blk_t **pp = &free_list;
    for(blk_t *b = free_list; b; pp = &b->next_free, b = b->next_free){
        if(b->size < n) continue;
        if(b->size >= n + HDR + 64){
            /* split: the tail stays free, in the same place in the list */
            blk_t *rest = (blk_t *)((u8 *)b + HDR + n);
            rest->size = b->size - n - HDR;
            rest->free = 1;
            rest->next_free = b->next_free;
            b->size = n;
            *pp = rest;
        } else {
            *pp = b->next_free;
        }
        b->free = 0;
        b->next_free = NULL;
        return (u8 *)b + HDR;
    }
    return NULL;
}

void kfree(void *ptr){
    if(!ptr) return;
    blk_t *b = (blk_t *)((u8 *)ptr - HDR);
    if(b->free) return;
    b->free = 1;
    /* insert in address order, then merge with the neighbours */
    blk_t *prev = NULL, *cur = free_list;
    while(cur && cur < b){ prev = cur; cur = cur->next_free; }
    b->next_free = cur;
    if(prev) prev->next_free = b; else free_list = b;
    if(cur && after(b) == cur){ b->size += HDR + cur->size; b->next_free = cur->next_free; }
    if(prev && after(prev) == b){ prev->size += HDR + b->size; prev->next_free = b->next_free; }
}

void *kcalloc(size_t n, size_t sz){
    size_t t = n * sz;
    void *p = kmalloc(t);
    if(p) memset(p, 0, t);
    return p;
}

void *krealloc(void *ptr, size_t size){
    if(!ptr) return kmalloc(size);
    blk_t *b = (blk_t *)((u8 *)ptr - HDR);
    if(b->size >= size) return ptr;
    void *n = kmalloc(size);
    if(!n) return NULL;
    memcpy(n, ptr, b->size);
    kfree(ptr);
    return n;
}
