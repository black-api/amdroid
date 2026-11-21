/* glue.c — what Reel needs from a C library that musl cannot give it here.
 *
 * Reel links musl's libc.a for the pure parts (string functions, libm, snprintf, strtod).
 * musl's own startup, malloc and syscall wrappers assume Linux: they would set up TLS with
 * arch_prctl and get memory with brk/mmap. Reel has neither, so:
 *  - memory comes from one heap the kernel maps at REEL_HEAP_VA (the allocator below),
 *  - errno is a plain variable (musl's lives in the thread block, reached through %fs),
 *  - the file, clock and thread calls the codec libraries link but never make on this path
 *    fail politely instead of executing a Linux `syscall` instruction.
 * tools/codecs/check-reel.sh lists every %fs access and syscall left in the final binary. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <sched.h>
#include <pthread.h>

void reel_fatal(const char *msg);

/* ---------------- the allocator ----------------
 * Boundary tags with segregated free lists. Every block has a 16-byte header holding its
 * size and the size of the block before it, so neighbours merge in O(1) on free. Free
 * blocks are kept in 64 bins by size; a request takes the first block that fits from the
 * smallest bin that can hold it. Payloads are 16-byte aligned; aligned allocation splits
 * the front off as a free block. */
typedef struct hdr {
    uint64_t size;         /* bytes including this header; bit 0 = in use */
    uint64_t prev_size;    /* size of the block before (0 for the first) */
} hdr_t;
typedef struct fblk {
    hdr_t h;
    struct fblk *next, *prev;
} fblk_t;

#define INUSE 1ull
#define HSZ   16ull
#define MINBLK 48ull                     /* header + two list pointers, rounded */
#define NBINS 64
static fblk_t *bins[NBINS];
static uint8_t *heap_lo, *heap_hi;
static uint64_t heap_used, heap_peak;

static inline uint64_t bsize(const hdr_t *h){ return h->size & ~15ull; }
static inline hdr_t *next_of(hdr_t *h){ return (hdr_t *)((uint8_t *)h + bsize(h)); }
static inline hdr_t *prev_of(hdr_t *h){ return (hdr_t *)((uint8_t *)h - h->prev_size); }

static int bin_of(uint64_t sz){
    /* 16-byte steps up to 512, then 4 bins per power of two */
    if(sz <= 512) return (int)(sz >> 4) - 3 < 0 ? 0 : (int)(sz >> 4) - 3;   /* 0..29 */
    int lg = 63 - __builtin_clzll(sz);                                      /* >= 9 */
    int b = 30 + (lg - 9) * 4 + (int)((sz >> (lg - 2)) & 3);
    return b < NBINS ? b : NBINS - 1;
}
static void bin_add(fblk_t *f){
    int b = bin_of(bsize(&f->h));
    f->prev = NULL;
    f->next = bins[b];
    if(bins[b]) bins[b]->prev = f;
    bins[b] = f;
}
static void bin_del(fblk_t *f){
    if(f->prev) f->prev->next = f->next;
    else bins[bin_of(bsize(&f->h))] = f->next;
    if(f->next) f->next->prev = f->prev;
}

void reel_heap_init(void *base, uint64_t bytes){
    heap_lo = (uint8_t *)(((uintptr_t)base + 15) & ~15ull);
    heap_hi = (uint8_t *)base + bytes;
    heap_hi = (uint8_t *)((uintptr_t)heap_hi & ~15ull);
    /* one free block, then a 16-byte in-use sentinel at the end */
    hdr_t *first = (hdr_t *)heap_lo;
    uint64_t total = (uint64_t)(heap_hi - heap_lo) - HSZ;
    first->size = total;
    first->prev_size = 0;
    hdr_t *end = (hdr_t *)(heap_lo + total);
    end->size = HSZ | INUSE;
    end->prev_size = total;
    bin_add((fblk_t *)first);
}

uint64_t reel_heap_used(void){ return heap_used; }
uint64_t reel_heap_peak(void){ return heap_peak; }

/* splits block h (in use, size >= need) so it is `need` bytes; the rest becomes free */
static void split(hdr_t *h, uint64_t need){
    uint64_t sz = bsize(h);
    if(sz - need < MINBLK) return;
    hdr_t *rest = (hdr_t *)((uint8_t *)h + need);
    rest->size = sz - need;
    rest->prev_size = need;
    next_of(rest)->prev_size = sz - need;
    h->size = need | INUSE;
    /* the rest may touch a free block after it */
    hdr_t *nx = next_of(rest);
    if(!(nx->size & INUSE)){
        bin_del((fblk_t *)nx);
        rest->size += bsize(nx);
        next_of(rest)->prev_size = bsize(rest);
    }
    bin_add((fblk_t *)rest);
}

static void *take(uint64_t need){
    for(int b = bin_of(need); b < NBINS; b++){
        for(fblk_t *f = bins[b]; f; f = f->next){
            if(bsize(&f->h) < need) continue;
            bin_del(f);
            f->h.size |= INUSE;
            split(&f->h, need);
            heap_used += bsize(&f->h);
            if(heap_used > heap_peak) heap_peak = heap_used;
            return (uint8_t *)f + HSZ;
        }
    }
    return NULL;
}

static uint64_t round_req(size_t n){
    uint64_t need = ((uint64_t)n + HSZ + 15) & ~15ull;
    return need < MINBLK ? MINBLK : need;
}

void *malloc(size_t n){
    if(n > (1ull << 40)) { errno = ENOMEM; return NULL; }
    void *p = take(round_req(n));
    if(!p) errno = ENOMEM;
    return p;
}

void free(void *p){
    if(!p) return;
    hdr_t *h = (hdr_t *)((uint8_t *)p - HSZ);
    if(!(h->size & INUSE) || (uint8_t *)h < heap_lo || (uint8_t *)h >= heap_hi) reel_fatal("free: not a heap block");
    heap_used -= bsize(h);
    h->size &= ~INUSE;
    hdr_t *nx = next_of(h);
    if(!(nx->size & INUSE)){
        bin_del((fblk_t *)nx);
        h->size += bsize(nx);
    }
    if(h->prev_size){
        hdr_t *pv = prev_of(h);
        if(!(pv->size & INUSE)){
            bin_del((fblk_t *)pv);
            pv->size += bsize(h);
            h = pv;
        }
    }
    next_of(h)->prev_size = bsize(h);
    bin_add((fblk_t *)h);
}

void *calloc(size_t a, size_t b){
    if(b && a > (size_t)-1 / b){ errno = ENOMEM; return NULL; }
    void *p = malloc(a * b);
    if(p) memset(p, 0, a * b);
    return p;
}

void *realloc(void *p, size_t n){
    if(!p) return malloc(n);
    if(!n){ free(p); return NULL; }
    hdr_t *h = (hdr_t *)((uint8_t *)p - HSZ);
    uint64_t need = round_req(n), have = bsize(h);
    if(need <= have){
        heap_used -= have;
        split(h, need);
        heap_used += bsize(h);
        return p;
    }
    hdr_t *nx = next_of(h);
    if(!(nx->size & INUSE) && have + bsize(nx) >= need){
        bin_del((fblk_t *)nx);
        heap_used -= have;
        h->size += bsize(nx);
        next_of(h)->prev_size = bsize(h);
        split(h, need);
        heap_used += bsize(h);
        if(heap_used > heap_peak) heap_peak = heap_used;
        return p;
    }
    void *q = malloc(n);
    if(!q) return NULL;
    memcpy(q, p, have - HSZ);
    free(p);
    return q;
}

void *aligned_alloc(size_t al, size_t n){
    if(al <= 16) return malloc(n);
    if(al & (al - 1)){ errno = EINVAL; return NULL; }
    uint64_t need = round_req(n);
    uint8_t *raw = malloc(need + al + MINBLK);
    if(!raw) return NULL;
    uintptr_t want = ((uintptr_t)raw + MINBLK + al - 1) & ~(uintptr_t)(al - 1);
    if((uintptr_t)raw % al == 0) want = (uintptr_t)raw;
    if(want == (uintptr_t)raw){ return raw; }
    /* give the front back as a free block of its own */
    hdr_t *h = (hdr_t *)(raw - HSZ);
    uint64_t total = bsize(h);
    uint64_t front = (uint64_t)(want - (uintptr_t)raw);
    hdr_t *nh = (hdr_t *)((uint8_t *)h + front);
    nh->size = (total - front) | INUSE;
    nh->prev_size = front;
    next_of(nh)->prev_size = total - front;
    h->size = front | INUSE;
    free(raw);                     /* the front: merges with a free neighbour before it */
    uint64_t before = bsize(nh);   /* and the slack at the end goes back too */
    split(nh, need);
    heap_used -= before - bsize(nh);
    return (void *)want;
}

int posix_memalign(void **out, size_t al, size_t n){
    void *p = aligned_alloc(al < sizeof(void *) ? sizeof(void *) : al, n);
    if(!p) return ENOMEM;
    *out = p;
    return 0;
}
void *memalign(size_t al, size_t n){ return aligned_alloc(al, n); }

/* ---------------- errno without TLS ---------------- */
static int errno_value;
int *__errno_location(void){ return &errno_value; }
int *___errno_location(void){ return &errno_value; }

/* strerror reaches the locale through the thread block: answer without it */
char *strerror(int e){ static char b[24]; snprintf(b, sizeof b, "error %d", e); return b; }
int strerror_r(int e, char *buf, size_t n){ snprintf(buf, n, "error %d", e); return 0; }
int __xpg_strerror_r(int e, char *buf, size_t n){ return strerror_r(e, buf, n); }

/* ---------------- what must never run here ---------------- */
void abort(void){ reel_fatal("abort() called"); for(;;){} }
void exit(int c){ (void)c; reel_fatal("exit() called"); for(;;){} }
void _Exit(int c){ exit(c); }
char *getenv(const char *n){ (void)n; return NULL; }
int isatty(int fd){ (void)fd; return 0; }
int open(const char *p, int f, ...){ (void)p; (void)f; errno = ENOSYS; return -1; }
int open64(const char *p, int f, ...){ (void)p; (void)f; errno = ENOSYS; return -1; }
int fcntl(int fd, int c, ...){ (void)fd; (void)c; errno = ENOSYS; return -1; }
int close(int fd){ (void)fd; return 0; }
long sysconf(int n){ (void)n; return -1; }
unsigned long getauxval(unsigned long t){ (void)t; return 0; }
int sched_getaffinity(int pid, size_t sz, cpu_set_t *set){ (void)pid; if(set) memset(set, 0, sz); errno = ENOSYS; return -1; }
int pthread_getaffinity_np(pthread_t th, size_t sz, cpu_set_t *set){ (void)th; if(set) memset(set, 0, sz); return ENOSYS; }
int nanosleep(const struct timespec *a, struct timespec *b){ (void)a; (void)b; return 0; }
int usleep(unsigned u){ (void)u; return 0; }
/* time: Reel keeps its own (the kernel's clock comes through shared memory) */
int clock_gettime(clockid_t c, struct timespec *ts){ (void)c; ts->tv_sec = 0; ts->tv_nsec = 0; return 0; }
int gettimeofday(struct timeval *tv, void *tz){ (void)tz; if(tv){ tv->tv_sec = 0; tv->tv_usec = 0; } return 0; }
time_t time(time_t *t){ if(t) *t = 0; return 0; }
clock_t clock(void){ return 0; }
/* stdio to files: av_log goes to our callback instead. (vfprintf itself must stay musl's:
 * snprintf and vsnprintf are built on it.) */
int fputs(const char *s, FILE *f){ (void)s; (void)f; return 0; }
int fputc(int c, FILE *f){ (void)f; return c; }
int fflush(FILE *f){ (void)f; return 0; }
int fprintf(FILE *f, const char *fmt, ...){ (void)f; (void)fmt; return 0; }
int printf(const char *fmt, ...){ (void)fmt; return 0; }
int puts(const char *s){ (void)s; return 0; }
int setvbuf(FILE *f, char *b, int m, size_t s){ (void)f; (void)b; (void)m; (void)s; return 0; }
FILE *fopen(const char *p, const char *m){ (void)p; (void)m; errno = ENOSYS; return NULL; }
FILE *fdopen(int fd, const char *m){ (void)fd; (void)m; errno = ENOSYS; return NULL; }
int fclose(FILE *f){ (void)f; return 0; }
size_t fread(void *p, size_t a, size_t b, FILE *f){ (void)p; (void)a; (void)b; (void)f; return 0; }
int mkstemp(char *t){ (void)t; errno = ENOSYS; return -1; }
int mkstemp64(char *t){ (void)t; errno = ENOSYS; return -1; }
int raise(int s){ (void)s; reel_fatal("raise() called"); return 0; }

/* musl's own code (time zones) allocates through these names: the same heap */
void *__libc_malloc(size_t n){ return malloc(n); }
void *__libc_malloc_impl(size_t n){ return malloc(n); }
void *__libc_calloc(size_t a, size_t b){ return calloc(a, b); }
void *__libc_realloc(void *p, size_t n){ return realloc(p, n); }
void __libc_free(void *p){ free(p); }

/* ---------------- threads: Reel has exactly one ----------------
 * dav1d is built with its thread pool and asks for it only when n_threads > 1 (codec.c
 * sets 1), but it still takes locks and runs its one-time init through these. musl's
 * versions reach the thread block through %fs on their slow paths, so they are replaced
 * by what one thread needs: nothing to wait for, nobody to wake. */
#include <pthread.h>
int pthread_once(pthread_once_t *o, void (*fn)(void)){ if(*o == 0){ *o = 1; fn(); } return 0; }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a){ (void)a; memset(m, 0, sizeof *m); return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m){ (void)m; return 0; }
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a){ (void)a; memset(c, 0, sizeof *c); return 0; }
int pthread_cond_destroy(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_signal(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m){ (void)c; (void)m; reel_fatal("pthread_cond_wait with one thread"); return 0; }
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg){ (void)t; (void)a; (void)fn; (void)arg; return EAGAIN; }
int pthread_join(pthread_t t, void **r){ (void)t; (void)r; return ESRCH; }
int pthread_attr_init(pthread_attr_t *a){ memset(a, 0, sizeof *a); return 0; }
int pthread_attr_destroy(pthread_attr_t *a){ (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s){ (void)a; (void)s; return 0; }
pthread_t pthread_self(void){ return (pthread_t)1; }
