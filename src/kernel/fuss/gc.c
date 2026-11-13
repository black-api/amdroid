/* gc.c — arena and conservative mark-sweep collector for Fuss runtime values
 * (strings, arrays, structs). The AST and compile tables use the kernel heap
 * and are never collected.
 *
 * Roots are found by the caller (eval.c): the value stack, the globals, and
 * the C stack. Every aligned word that points into a live block keeps it.
 * Interior pointers count, so a pointer into the middle of a string is safe. */
#include "fuss_int.h"
#include "amdroid/heap.h"
#include <string.h>

#define FZ_ARENA_BYTES (16u << 20)
#define HDR 16u
#define F_USED 1u
#define F_MARK 2u

typedef struct Blk { u32 size; u32 flags; struct Blk *next; } Blk;   /* next: free list only */

static unsigned char *arena_lo, *arena_hi;
static Blk *freelist;

static Blk **blkindex;          /* all blocks, in address order (collection only) */
static size_t nblk;
static Blk **mstack;            /* mark stack */
static size_t nmstack, capmstack;

static void arena_init(void){
    /* Prefer a big arena, but fall back to smaller blocks: the kernel heap is
     * much fuller when a program is launched from the GUI (the framebuffer
     * back-buffer alone is several MB) than from the empty-heap text shell. */
    u32 want = FZ_ARENA_BYTES;
    unsigned char *mem = NULL;
    while(want >= (2u << 20)){                 /* down to 2 MB */
        mem = kmalloc(want + 16);
        if(mem) break;
        want /= 2;
    }
    if(!mem) fz_die("out of memory (interpreter arena)");
    arena_lo = (unsigned char*)(((unsigned long)mem + 15) & ~15ul);
    arena_hi = arena_lo + want;
    Blk *b = (Blk*)arena_lo;
    b->size = want - HDR;
    b->flags = 0;
    b->next = NULL;
    freelist = b;
}

static void *try_alloc(size_t n){
    size_t need = (n + 15) & ~(size_t)15;
    if(need < 16) need = 16;
    Blk **pp = &freelist;
    for(Blk *b = freelist; b; pp = &b->next, b = b->next){
        if(b->size < need) continue;
        if(b->size >= need + HDR + 16){
            /* split: the tail stays free in b's place on the list */
            Blk *r = (Blk*)((unsigned char*)(b + 1) + need);
            r->size = (u32)(b->size - need - HDR);
            r->flags = 0;
            r->next = b->next;
            b->size = (u32)need;
            *pp = r;
        } else {
            *pp = b->next;
        }
        b->flags = F_USED;
        b->next = NULL;
        memset(b + 1, 0, b->size);
        return b + 1;
    }
    return NULL;
}

/* Allocates zeroed memory for a runtime value. Collects garbage when full. */
void *fz_alloc(size_t n){
    if(!arena_lo) arena_init();
    void *p = try_alloc(n);
    if(p) return p;
    fz_gc_collect();
    p = try_alloc(n);
    if(!p) fz_die("out of memory (%u bytes needed)", (unsigned)n);
    return p;
}

void *fz_realloc_copy(void *old, size_t oldn, size_t newn){
    void *p = fz_alloc(newn);
    if(old && oldn) memcpy(p, old, oldn < newn ? oldn : newn);
    return p;
}

char *fz_strndup(const char *s, size_t n){
    size_t l = 0;
    while(l < n && s[l]) l++;
    char *d = fz_alloc(l + 1);
    memcpy(d, s, l);
    d[l] = 0;
    return d;
}
char *fz_strdup(const char *s){ return fz_strndup(s, (size_t)-1 >> 1); }

/* ---------- collection primitives ---------- */

void fz_gc_begin(void){
    size_t n = 0;
    for(Blk *b = (Blk*)arena_lo; (unsigned char*)b < arena_hi; b = (Blk*)((unsigned char*)(b + 1) + b->size)) n++;
    blkindex = krealloc(blkindex, n * sizeof(Blk*));
    if(!blkindex) fz_die("out of memory (gc index)");
    nblk = 0;
    for(Blk *b = (Blk*)arena_lo; (unsigned char*)b < arena_hi; b = (Blk*)((unsigned char*)(b + 1) + b->size))
        blkindex[nblk++] = b;
    nmstack = 0;
}

static void push_mark(Blk *b){
    if(nmstack == capmstack){
        capmstack = capmstack ? capmstack * 2 : 256;
        mstack = krealloc(mstack, capmstack * sizeof(Blk*));
        if(!mstack) fz_die("out of memory (gc stack)");
    }
    mstack[nmstack++] = b;
}

/* Marks the used block that contains address w, if there is one. */
static void consider(unsigned long w){
    if(w < (unsigned long)arena_lo || w >= (unsigned long)arena_hi) return;
    size_t lo = 0, hi = nblk;                 /* last block starting at or before w */
    while(hi - lo > 1){
        size_t mid = (lo + hi) / 2;
        if((unsigned long)blkindex[mid] <= w) lo = mid; else hi = mid;
    }
    Blk *b = blkindex[lo];
    if(!(b->flags & F_USED) || (b->flags & F_MARK)) return;
    if(w >= (unsigned long)(b + 1) + b->size) return;
    b->flags |= F_MARK;
    push_mark(b);
}

void fz_gc_mark_range(const void *lo, const void *hi){
    unsigned long a = ((unsigned long)lo + 7) & ~7ul;
    unsigned long end = (unsigned long)hi;
    for(; a + 8 <= end; a += 8) consider(*(unsigned long*)a);
}

void fz_gc_drain(void){
    while(nmstack){
        Blk *b = mstack[--nmstack];
        unsigned long p = (unsigned long)(b + 1);
        for(u32 i = 0; i + 8 <= b->size; i += 8) consider(*(unsigned long*)(p + i));
    }
}

/* Frees every unmarked block and rebuilds the free list (coalescing neighbours). */
void fz_gc_sweep(void){
    freelist = NULL;
    Blk *tail = NULL;
    Blk *b = (Blk*)arena_lo;
    while((unsigned char*)b < arena_hi){
        Blk *nb = (Blk*)((unsigned char*)(b + 1) + b->size);
        if(b->flags & F_USED){
            if(b->flags & F_MARK) b->flags &= ~F_MARK;
            else b->flags = 0;
        }
        if(!(b->flags & F_USED)){
            while((unsigned char*)nb < arena_hi && !(nb->flags & F_USED)){
                b->size += HDR + nb->size;
                nb = (Blk*)((unsigned char*)(b + 1) + b->size);
            }
            b->next = NULL;
            if(tail) tail->next = b; else freelist = b;
            tail = b;
        }
        b = nb;
    }
}
