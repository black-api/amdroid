#include "amdroid/vmm.h"
#include "amdroid/pmm.h"
#include "amdroid/types.h"
#include "amdroid/console.h"

extern unsigned long pml4[512];

static unsigned long *get_table(unsigned long *parent, int index){
    if(!(parent[index] & 1)){
        void *p = pmm_alloc_page();
        if(!p) return NULL;
        /* zero */
        unsigned long *t = (unsigned long*)p; /* identity mapped */
        for(int i=0;i<512;i++) t[i]=0;
        parent[index] = (unsigned long)p | 0x3; /* present, writable */
        return t;
    }
    return (unsigned long*)(parent[index] & ~0xFFF);
}

void vmm_map_region(u64 virt, u64 phys, u64 size){
    u64 v = virt & ~0xFFF;
    u64 p = phys & ~0xFFF;
    u64 end = virt + size;
    while(v < end){
        int i4 = (v >> 39) & 0x1FF;
        int i3 = (v >> 30) & 0x1FF;
        int i2 = (v >> 21) & 0x1FF;
        int i1 = (v >> 12) & 0x1FF;
        unsigned long *pdpt = get_table(pml4, i4);
        if(!pdpt) return;
        unsigned long *pd = get_table(pdpt, i3);
        if(!pd) return;
        /* use 4K pages */
        unsigned long *pt = get_table(pd, i2);
        if(!pt) return;
        pt[i1] = p | 0x3; /* present, writable */
        v += 0x1000;
        p += 0x1000;
    }
    /* flush TLB */
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4) : "memory");
}
