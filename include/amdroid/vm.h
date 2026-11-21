#ifndef AMDROID_VM_H
#define AMDROID_VM_H
#include "amdroid/types.h"

/* User programs live in [VM_USER_BASE, VM_USER_BASE + 1GB). Each address
   space shares the kernel mappings, but the user region is private. */
#define VM_USER_BASE  0x40000000ULL
#define VM_USER_LIMIT 0x80000000ULL

#define PTE_P 0x1ULL
#define PTE_W 0x2ULL
#define PTE_U 0x4ULL
#define PTE_KEEP 0x200ULL            /* a borrowed page: not private to this process, never freed */

u64  vm_kernel_cr3(void);
u64  vm_new_space(void);                              /* returns the new PML4 (physical) */
int  vm_map_user(u64 cr3, u64 virt, u64 phys, u64 flags);
u64  vm_alloc_user_page(u64 cr3, u64 virt);           /* allocates and maps one zeroed page */
/* User programs are linked at the base; keep the signal trampoline far
 * above any realistic image's end (text + rodata + data + bss), not inside it. */
#define VM_SIG_TRAMPOLINE (VM_USER_BASE + 0x1FFF0000)  /* just below 0x60000000 (Husser's shared region) */
void vm_map_trampoline(u64 cr3);
void vm_switch(u64 cr3);
u64  vm_read_cr3(void);
void vm_free_space(u64 cr3);                          /* releases a process's memory */
int  vm_copy_to_user(u64 cr3, u64 va, const void *src, u64 len);
int  vm_copy_from_user(u64 cr3, u64 va, void *dst, u64 len);
u64  vm_user_pages(u64 cr3);                        /* resident pages of a process's own region */
u64  vm_lookup(u64 cr3, u64 virt);                    /* physical page or 0 */
/* Demand paging: a process's big, mostly empty regions (heaps, stacks) are not backed until
 * touched. vm_lazy_range registers [lo, hi) for this address space; the first touch of a page
 * (a user page fault, or a system call copying to it) maps a zeroed page. vm_demand returns 1
 * when the page is now there. */
void vm_lazy_range(u64 cr3, u64 lo, u64 hi);
int  vm_demand(u64 cr3, u64 va);

#endif
