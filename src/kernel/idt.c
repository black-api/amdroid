#include "amdroid/idt.h"
#include "amdroid/console.h"
#include "amdroid/types.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"

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

/* extern stubs */
#define DECL(n) extern void isr_##n(void);
DECL(0) DECL(1) DECL(2) DECL(3) DECL(4) DECL(5) DECL(6) DECL(7)
DECL(8) DECL(9) DECL(10) DECL(11) DECL(12) DECL(13) DECL(14) DECL(15)
DECL(16) DECL(17) DECL(18) DECL(19) DECL(20) DECL(21) DECL(22) DECL(23)
DECL(24) DECL(25) DECL(26) DECL(27) DECL(28) DECL(29) DECL(30) DECL(31)
DECL(32) DECL(33) DECL(34) DECL(35) DECL(36) DECL(37) DECL(38) DECL(39)
DECL(40) DECL(41) DECL(42) DECL(43) DECL(44) DECL(45) DECL(46) DECL(47)
extern void isr_128(void);

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
    for(int i=0;i<48;i++) idt_set_gate(i, (u64)isr_table[i], 0x08, 0x8E);
    idt_set_gate(128, (u64)isr_128, 0x08, 0x8E);
    __asm__ volatile("lidt %0"::"m"(idtp));
}

/* ISR handler called from stub. Frame layout: vector at [rsp+?]. 
   We passed rsp (pointing to r15.. then vector, errcode). Let's parse. */
struct isr_frame {
    /* pushed by stub in order: r15..rax then? Actually stub pushes rax..r15? Let's check order */
};

/* Simpler: handler receives pointer to saved regs; vector is after regs. */
void isr_handler(u64 *frame){
    /* frame points to r15 (last pushed? we pushed rax first then ... r15, so top is r15). 
       After all 15 regs (15*8=120), next is vector, then errcode. */
    u64 vector = frame[15]; /* after 15 regs */
    /* dispatch */
    if(vector==32){ timer_tick(); }
    if(vector==33){ keyboard_irq(); }
    /* EOI for IRQs */
    if(vector>=32 && vector<=47){
        if(vector>=40) { /* slave */ 
            /* outb 0xA0,0x20 */
            __asm__ volatile("outb %0,%1"::"a"((u8)0x20),"Nd"((u16)0xA0));
        }
        __asm__ volatile("outb %0,%1"::"a"((u8)0x20),"Nd"((u16)0x20));
    }
    if(vector==14){
        u64 cr2; __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
        console_printf("PAGE FAULT at %p\n", (void*)cr2);
        for(;;) __asm__ volatile("hlt");
    }
    if(vector==13){
        console_printf("GPF\n");
    }
}
