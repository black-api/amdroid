/* vm.c — per-process address spaces.
 *
 * Layout of a process PML4:
 *   PML4[0] -> private PDPT (U=1). Entries 0..3 of the PDPT point at the
 *              kernel's page directories, which have no U bit, so user code
 *              cannot touch kernel memory. Entry 1 (1-2 GB) is private and
 *              holds the user region.
 *   PML4[1..511] are shared with the kernel.
 */
#include "amdroid/vm.h"
#include "amdroid/pmm.h"
#include "amdroid/types.h"
#include "amdroid/pmm.h"
#include <string.h>

static u64 kernel_cr3;

u64 vm_read_cr3(void){
    u64 v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v & ~0xFFFULL;
}

static u64 *table_new(void){
    u64 p = (u64)(unsigned long)pmm_alloc_page();   /* identity mapped */
    if(!p) return NULL;
    memset((void*)(unsigned long)p, 0, 4096);
    return (u64*)(unsigned long)p;
}

u64 vm_kernel_cr3(void){
    if(!kernel_cr3) kernel_cr3 = vm_read_cr3();
    return kernel_cr3;
}

u64 vm_new_space(void){
    u64 kcr3 = vm_kernel_cr3();
    u64 *kpml4 = (u64*)(unsigned long)kcr3;
    u64 *pml4 = table_new();
    u64 *pdpt = table_new();
    if(!pml4 || !pdpt) return 0;
    /* share the kernel's page directories through a private PDPT */
    u64 *kpdpt = (u64*)(unsigned long)(kpml4[0] & ~0xFFFULL);
    for(int i = 0; i < 512; i++) pdpt[i] = kpdpt[i];
    pdpt[1] = 0;                                    /* private user region */
    pml4[0] = (u64)(unsigned long)pdpt | PTE_P | PTE_W | PTE_U;
    for(int i = 1; i < 512; i++) pml4[i] = kpml4[i];
    return (u64)(unsigned long)pml4;
}

/* Returns the next-level table at index, creating it if needed. */
static u64 *walk(u64 *tbl, int idx){
    if(!(tbl[idx] & PTE_P)){
        u64 *t = table_new();
        if(!t) return NULL;
        tbl[idx] = (u64)(unsigned long)t | PTE_P | PTE_W | PTE_U;
    }
    return (u64*)(unsigned long)(tbl[idx] & ~0xFFFULL);
}

int vm_map_user(u64 cr3, u64 virt, u64 phys, u64 flags){
    u64 *pml4 = (u64*)(unsigned long)cr3;
    u64 *pdpt = walk(pml4, (virt >> 39) & 511);
    if(!pdpt) return -1;
    u64 *pd = walk(pdpt, (virt >> 30) & 511);
    if(!pd) return -1;
    u64 *pt = walk(pd, (virt >> 21) & 511);
    if(!pt) return -1;
    pt[(virt >> 12) & 511] = (phys & ~0xFFFULL) | flags | PTE_P;
    return 0;
}

u64 vm_alloc_user_page(u64 cr3, u64 virt){
    u64 p = (u64)(unsigned long)pmm_alloc_page();
    if(!p) return 0;
    memset((void*)(unsigned long)p, 0, 4096);
    if(vm_map_user(cr3, virt, p, PTE_W | PTE_U) != 0) return 0;
    return p;
}

void vm_switch(u64 cr3){
    __asm__ volatile("movq %0, %%cr3" :: "r"(cr3) : "memory");
}

/* Returns the physical page mapped at virt in cr3, or 0. */
u64 vm_lookup(u64 cr3, u64 virt){
    u64 *pml4 = (u64*)(unsigned long)cr3;
    if(!(pml4[(virt >> 39) & 511] & PTE_P)) return 0;
    u64 *pdpt = (u64*)(unsigned long)(pml4[(virt >> 39) & 511] & ~0xFFFULL);
    if(!(pdpt[(virt >> 30) & 511] & PTE_P)) return 0;
    u64 *pd = (u64*)(unsigned long)(pdpt[(virt >> 30) & 511] & ~0xFFFULL);
    if(!(pd[(virt >> 21) & 511] & PTE_P)) return 0;
    u64 *pt = (u64*)(unsigned long)(pd[(virt >> 21) & 511] & ~0xFFFULL);
    u64 e = pt[(virt >> 12) & 511];
    return (e & PTE_P) ? (e & ~0xFFFULL) : 0;
}

/* Frees everything a process owns: its private user region (pages and page
 * tables), its private PDPT and its PML4. The kernel's shared directories are
 * left alone. The space must not be the one loaded in CR3. */
void vm_free_space(u64 cr3){
    u64 *pml4 = (u64*)(unsigned long)cr3;
    if(!pml4) return;
    if(pml4[0] & PTE_P){
        u64 *pdpt = (u64*)(unsigned long)(pml4[0] & ~0xFFFULL);
        if(pdpt[1] & PTE_P){
            u64 *pd = (u64*)(unsigned long)(pdpt[1] & ~0xFFFULL);
            for(int i = 0; i < 512; i++){
                if(!(pd[i] & PTE_P)) continue;
                u64 *pt = (u64*)(unsigned long)(pd[i] & ~0xFFFULL);
                for(int j = 0; j < 512; j++)
                    if(pt[j] & PTE_P) pmm_free_page((void*)(unsigned long)(pt[j] & ~0xFFFULL));
                pmm_free_page(pt);
            }
            pmm_free_page(pd);
        }
        pmm_free_page(pdpt);
    }
    pmm_free_page(pml4);
}

/* Copies bytes into or out of a user address space, one page at a time. */
int vm_copy_to_user(u64 cr3, u64 va, const void *src, u64 len){
    const u8 *s = src;
    while(len){
        u64 page = vm_lookup(cr3, va & ~0xFFFULL);
        if(!page) return -1;
        u64 off = va & 0xFFF;
        u64 n = 4096 - off;
        if(n > len) n = len;
        memcpy((void*)(unsigned long)(page + off), s, (size_t)n);
        s += n; va += n; len -= n;
    }
    return 0;
}

int vm_copy_from_user(u64 cr3, u64 va, void *dst, u64 len){
    u8 *d = dst;
    while(len){
        u64 page = vm_lookup(cr3, va & ~0xFFFULL);
        if(!page) return -1;
        u64 off = va & 0xFFF;
        u64 n = 4096 - off;
        if(n > len) n = len;
        memcpy(d, (void*)(unsigned long)(page + off), (size_t)n);
        d += n; va += n; len -= n;
    }
    return 0;
}

/* The page every process maps for signal handlers: a call to sigreturn, then
 * a spin (sigreturn never returns). */
void vm_map_trampoline(u64 cr3){
    static const u8 code[] = {
        0x48, 0xC7, 0xC0, 0x0E, 0x00, 0x00, 0x00,   /* mov $14, %rax  (SYS_SIGRETURN) */
        0xCD, 0x80,                                 /* int $0x80 */
        0xEB, 0xFE                                  /* jmp . */
    };
    u64 p = vm_alloc_user_page(cr3, VM_SIG_TRAMPOLINE);
    if(p) memcpy((void*)(unsigned long)p, code, sizeof code);
}
