/* gdt.c — the 64-bit GDT: kernel and user segments, and the TSS. The TSS
 * supplies the kernel stack (rsp0) for interrupts that arrive in user mode. */
#include "amdroid/gdt.h"
#include "amdroid/types.h"

struct tss {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

static struct tss tss;
static u64 gdt[7] __attribute__((aligned(8)));

struct gdt_ptr { u16 limit; u64 base; } __attribute__((packed));

static void set_tss_desc(u64 base, u32 limit){
    /* a 64-bit system descriptor occupies two GDT slots */
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFFULL) << 16)
           | (0x89ULL << 40)                 /* present, DPL 0, 64-bit available TSS */
           | (((u64)limit >> 16 & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = (base >> 32) & 0xFFFFFFFFULL;
}

void gdt_init(void){
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code, 64-bit */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00AFFA000000FFFFULL;   /* 0x18 user code, DPL 3, 64-bit */
    gdt[4] = 0x00CFF2000000FFFFULL;   /* 0x20 user data, DPL 3 */
    tss.iomap_base = sizeof(tss);
    set_tss_desc((u64)&tss, sizeof(tss) - 1);

    struct gdt_ptr p = { sizeof(gdt) - 1, (u64)gdt };
    __asm__ volatile("lgdt %0" :: "m"(p));

    /* reload CS through a far return, then the data segments */
    __asm__ volatile(
        "pushq %0\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %1, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :: "i"(SEL_KCODE), "i"(SEL_KDATA) : "rax", "memory");

    __asm__ volatile("ltr %0" :: "r"((u16)SEL_TSS));
}

void gdt_set_rsp0(u64 rsp0){ tss.rsp0 = rsp0; }
