#include "amdroid/pmm.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

#define PAGE_SIZE 4096
/* the bitmap covers up to 4 GB (128 KiB); the real total comes from the
 * multiboot memory map, never from a constant */
#define PMM_MAX_MEM (4ull * 1024 * 1024 * 1024)     /* the bitmap's reach */
#define MAX_PAGES (PMM_MAX_MEM / PAGE_SIZE)         /* 2^20 pages */
#define BITMAP_BYTES (MAX_PAGES / 8)                /* 128 KiB */

static u8 bitmap[BITMAP_BYTES];
static u64 total_pages = 0;
static u64 free_pages = 0;
static u64 alloc_hint = 0;

static void bitmap_set(u64 i){ bitmap[i/8] |= (1<<(i%8)); }
static void bitmap_clear(u64 i){ bitmap[i/8] &= ~(1<<(i%8)); }
static int bitmap_test(u64 i){ return bitmap[i/8] & (1<<(i%8)); }

/* mark region [start,end) pages used */
static void mark_used(u64 start_page, u64 num){
    for(u64 i=0;i<num;i++) if(!bitmap_test(start_page+i)){ bitmap_set(start_page+i); if(free_pages) free_pages--; }
}
static void mark_free(u64 start_page, u64 num){
    for(u64 i=0;i<num;i++) if(bitmap_test(start_page+i)){ bitmap_clear(start_page+i); free_pages++; }
}

void pmm_init(u32 mbi){
    /* zero bitmap => all free initially */
    for(u64 i=0;i<BITMAP_BYTES;i++) bitmap[i]=0;
    free_pages=0; total_pages=0; alloc_hint=0;

    /* parse multiboot2 */
    u8 *ptr = (u8*)(u64)mbi;
    u32 total_size = *(u32*)ptr; (void)total_size;
    ptr += 8; /* skip total_size + reserved */
    u64 max_addr = 0;
    /* first pass: determine max available memory */
    u8 *end = (u8*)(u64)mbi + total_size;
    u8 *p = ptr;
    while(p < end){
        u32 type = *(u32*)p;
        u32 size = *(u32*)(p+4);
        if(size==0) break;
        if(type==6){ /* mmap */
            u32 entry_size = *(u32*)(p+8);
            /* entries start at p+16 */
            u8 *e = p+16;
            u8 *eend = p+size;
            while(e < eend){
                u64 base = *(u64*)e;
                u64 len = *(u64*)(e+8);
                u32 mtype = *(u32*)(e+16);
                if(mtype==1){
                    u64 top = base+len;
                    if(top>max_addr) max_addr=top;
                }
                e += entry_size;
            }
        }
        p += ALIGN_UP(size,8);
    }
    if(max_addr > PMM_MAX_MEM) max_addr = PMM_MAX_MEM;    /* the bitmap's reach */
    total_pages = max_addr / PAGE_SIZE;
    /* initially mark all pages used, then free available regions */
    for(u64 i=0;i<total_pages;i++){ bitmap_set(i); }
    free_pages = 0;

    /* second pass: free available regions */
    p = ptr;
    while(p < end){
        u32 type = *(u32*)p;
        u32 size = *(u32*)(p+4);
        if(size==0) break;
        if(type==6){
            u32 entry_size = *(u32*)(p+8);
            u8 *e = p+16;
            u8 *eend = p+size;
            while(e < eend){
                u64 base = *(u64*)e;
                u64 len = *(u64*)(e+8);
                u32 mtype = *(u32*)(e+16);
                if(mtype==1){
                    u64 sp = base/PAGE_SIZE;
                    u64 np = len/PAGE_SIZE;
                    if(sp+np > total_pages){ if(sp<total_pages) np=total_pages-sp; else np=0; }
                    mark_free(sp,np);
                }
                e += entry_size;
            }
        }
        p += ALIGN_UP(size,8);
    }

    /* reserve kernel region */
    extern char __kernel_start[], __kernel_end[];
    u64 ks = (u64)__kernel_start / PAGE_SIZE;
    u64 ke = ((u64)__kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    mark_used(0, ke); /* reserve up to kernel end (includes low mem) */
    (void)ks;
    /* also reserve pages for bitmap? bitmap is in BSS (kernel) already covered by kernel region; skip */

    console_printf("[pmm] total pages: %llu free: %llu (%llu MB)\n",
        (unsigned long long)total_pages, (unsigned long long)free_pages,
        (unsigned long long)(free_pages*4096/1024/1024));
}

/* The search starts where the last one ended (alloc_hint) and skips full bytes of the
 * bitmap, so handing out thousands of pages for a new process stays quick. */
void *pmm_alloc_page(void){
    if(!free_pages) return NULL;
    for(int pass = 0; pass < 2; pass++){
        u64 i = pass ? 0 : alloc_hint, stop = pass ? alloc_hint : total_pages;
        while(i < stop){
            if((i & 7) == 0 && bitmap[i / 8] == 0xFF){ i += 8; continue; }
            if(!bitmap_test(i)){
                bitmap_set(i);
                free_pages--;
                alloc_hint = i + 1;
                return (void*)(i*PAGE_SIZE);
            }
            i++;
        }
    }
    return NULL;
}

void pmm_free_page(void *p){
    u64 i = (u64)p / PAGE_SIZE;
    if(i < total_pages && bitmap_test(i)){
        bitmap_clear(i);
        free_pages++;
        if(i < alloc_hint) alloc_hint = i;
    }
}

u64 pmm_total_pages(void){ return total_pages; }
u64 pmm_free_pages(void){ return free_pages; }
