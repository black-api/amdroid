#include "amdroid/heap.h"
#include "amdroid/types.h"

#define HEAP_SIZE (32*1024*1024)
static char heap_buf[HEAP_SIZE];

typedef struct block {
    size_t size;
    int free;
    int owner;                  /* who asked for it: 0 the kernel, n+1 the GUI app n (heap_owner) */
    struct block *next;
} block_t;

int heap_owner = 0;             /* set by the GUI while an app's code runs */

static block_t *head = NULL;

void heap_init(void){
    head = (block_t*)heap_buf;
    head->size = HEAP_SIZE - sizeof(block_t);
    head->free = 1;
    head->next = NULL;
}

void *kmalloc(size_t size){
    if(size==0) return NULL;
    size = ALIGN_UP(size, 8);
    block_t *cur = head;
    while(cur){
        if(cur->free && cur->size >= size){
            if(cur->size >= size + sizeof(block_t) + 8){
                block_t *nb = (block_t*)((char*)cur + sizeof(block_t) + size);
                nb->size = cur->size - size - sizeof(block_t);
                nb->free = 1;
                nb->next = cur->next;
                cur->size = size;
                cur->next = nb;
            }
            cur->free = 0;
            cur->owner = heap_owner;
            return (char*)cur + sizeof(block_t);
        }
        cur = cur->next;
    }
    return NULL;
}

void kfree(void *ptr){
    if(!ptr) return;
    block_t *b = (block_t*)((char*)ptr - sizeof(block_t));
    b->free = 1;
    /* coalesce */
    block_t *cur = head;
    while(cur && cur->next){
        if(cur->free && cur->next->free){
            cur->size += sizeof(block_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* Bytes in use (with headers), in all and for each owner below nowners. */
void heap_stats(u64 *used, u64 *total, u64 *by_owner, int nowners){
    u64 u = 0;
    for(int i = 0; i < nowners; i++) by_owner[i] = 0;
    for(block_t *b = head; b; b = b->next){
        if(b->free) continue;
        u64 n = b->size + sizeof(block_t);
        u += n;
        if(b->owner >= 0 && b->owner < nowners) by_owner[b->owner] += n;
    }
    if(used) *used = u;
    if(total) *total = HEAP_SIZE;
}

void *kcalloc(size_t n, size_t sz){
    size_t t = n*sz;
    void *p = kmalloc(t);
    if(p) for(size_t i=0;i<t;i++) ((char*)p)[i]=0;
    return p;
}

void *krealloc(void *ptr, size_t size){
    if(!ptr) return kmalloc(size);
    if(size==0){ kfree(ptr); return NULL; }
    block_t *b = (block_t*)((char*)ptr - sizeof(block_t));
    if(b->size >= size) return ptr;
    void *np = kmalloc(size);
    if(np){ for(size_t i=0;i<b->size;i++) ((char*)np)[i]=((char*)ptr)[i]; kfree(ptr); }
    return np;
}
