#include "amdroid/idt.h"
#include "amdroid/console.h"
#include "amdroid/types.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"
#include "amdroid/gdt.h"
#include "amdroid/sched.h"
#include "amdroid/syscall.h"
#include "amdroid/vm.h"

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 reserved;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

/* extern stubs (isr_stubs.asm) */
#define DECL(n) extern void isr_##n(void);
DECL(0) DECL(1) DECL(2) DECL(3) DECL(4) DECL(5) DECL(6) DECL(7)
DECL(8) DECL(9) DECL(10) DECL(11) DECL(12) DECL(13) DECL(14) DECL(15)
DECL(16) DECL(17) DECL(18) DECL(19) DECL(20) DECL(21) DECL(22) DECL(23)
DECL(24) DECL(25) DECL(26) DECL(27) DECL(28) DECL(29) DECL(30) DECL(31)
DECL(32) DECL(33) DECL(34) DECL(35) DECL(36) DECL(37) DECL(38) DECL(39)
DECL(40) DECL(41) DECL(42) DECL(43) DECL(44) DECL(45) DECL(46) DECL(47)
DECL(128) DECL(129)

static void *isr_table[48] = {
    isr_0,isr_1,isr_2,isr_3,isr_4,isr_5,isr_6,isr_7,
    isr_8,isr_9,isr_10,isr_11,isr_12,isr_13,isr_14,isr_15,
    isr_16,isr_17,isr_18,isr_19,isr_20,isr_21,isr_22,isr_23,
    isr_24,isr_25,isr_26,isr_27,isr_28,isr_29,isr_30,isr_31,
    isr_32,isr_33,isr_34,isr_35,isr_36,isr_37,isr_38,isr_39,
    isr_40,isr_41,isr_42,isr_43,isr_44,isr_45,isr_46,isr_47,
};

void idt_set_gate(u8 n, u64 handler, u16 sel, u8 flags){
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = sel;
    idt[n].ist = 0;
    idt[n].type_attr = flags;
    idt[n].offset_mid = (handler>>16)&0xFFFF;
    idt[n].offset_high = (handler>>32)&0xFFFFFFFF;
    idt[n].reserved = 0;
}

void idt_init(void){
    idtp.limit = sizeof(idt)-1;
    idtp.base = (u64)idt;
    for(int i=0;i<256;i++) idt_set_gate(i,0,0,0);
    for(int i=0;i<48;i++) idt_set_gate(i, (u64)isr_table[i], SEL_KCODE, 0x8E);
    idt_set_gate(128, (u64)isr_128, SEL_KCODE, 0xEE);   /* int 0x80: callable from user mode */
    idt_set_gate(129, (u64)isr_129, SEL_KCODE, 0x8E);   /* int 0x81: kernel yield */
    __asm__ volatile("lidt %0"::"m"(idtp));
}

static inline void outb8(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

/* Called from isr_common with the saved frame (see isr_stubs.asm). Returns the
 * frame to resume: normally the same one, but a switch may pick another
 * process's frame. Frame layout: 0..14 GPRs (r15 first), 15 vector,
 * 16 error code, 17 rip, 18 cs, 19 rflags, 20 rsp, 21 ss. */
u64 *isr_handler(u64 *frame){
    u64 vec = frame[15];
    int from_user = (frame[18] & 3) == 3;
    u64 *next = frame;

    if(vec >= 32 && vec <= 47){
        if(vec >= 40) outb8(0xA0, 0x20);
        outb8(0x20, 0x20);          /* EOI first: a switch may not return soon */
    }

    switch(vec){
        case 32:
            timer_tick();
            next = sched_preempt(frame);
            break;
        case 33:
            keyboard_irq();
            break;
        case 128:
            next = syscall_dispatch(frame);
            break;
        case 129:
            next = sched_switch(frame);
            break;
        case 13:
        case 14: {
            u64 cr2 = 0;
            if(vec == 14) __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
            if(from_user){
                console_printf("[proc %d] killed: %s at rip=%p addr=%p\n", proc_current(),
                               vec == 14 ? "page fault" : "general protection fault",
                               (void*)frame[17], (void*)cr2);
                for(int k = 0; k < 22; k++) console_printf("  frame[%d] = %llx\n", k, (unsigned long long)frame[k]);
                {
                    u64 rip = frame[17];
                    const u8 *pb = (const u8 *)(unsigned long)rip;
                    console_printf("  rip bytes: %02x %02x %02x %02x %02x %02x\n", pb[0], pb[1], pb[2], pb[3], pb[4], pb[5]);
                    u64 cr3 = vm_read_cr3();
                    u64 va = rip & ~0xFFFULL;
                    u64 *pml4 = (u64 *)(unsigned long)cr3;
                    u64 *pdpt = (u64 *)(unsigned long)(pml4[(va >> 39) & 511] & ~0xFFFULL);
                    u64 *pd = pdpt ? (u64 *)(unsigned long)(pdpt[(va >> 30) & 511] & ~0xFFFULL) : 0;
                    u64 *pt = pd ? (u64 *)(unsigned long)(pd[(va >> 21) & 511] & ~0xFFFULL) : 0;
                    u64 pte = pt ? pt[(va >> 12) & 511] : 0;
                    console_printf("  walk: pml4 %llx pdpt %llx pd %llx pt %llx pte %llx\n",
                                   (unsigned long long)(u64)pml4, (unsigned long long)(u64)pdpt,
                                   (unsigned long long)(u64)pd, (unsigned long long)(u64)pt,
                                   (unsigned long long)pte);
                    console_printf("  cr3 %llx rip page phys %llx\n", (unsigned long long)cr3,
                                   (unsigned long long)vm_lookup(cr3, va));
                }
                next = proc_exit(frame, -1);
                break;
            }
            console_printf("KERNEL %s at rip=%p addr=%p\n",
                           vec == 14 ? "PAGE FAULT" : "GP FAULT", (void*)frame[17], (void*)cr2);
            for(;;) __asm__ volatile("cli; hlt");
        }
        default:
            break;
    }
    return next;
}
