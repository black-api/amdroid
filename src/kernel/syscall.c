#include "amdroid/syscall.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

void syscall_init(void){
    /* register gate 128 in idt - done in idt_init separately; here just announce */
}

void syscall_handler(u64 *frame){
    /* frame layout same as isr: regs then vector(128) then errcode(0) */
    /* We need syscall number. We pushed rax as first reg? In stub above, we push rax first then rbx... 
       Stack top (rsp) after all pushes = r15 slot. So frame[0]=r15,... frame[14]=rax? Wait order: push rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15. 
       Last pushed r15 is top. So frame[0]=r15, frame[1]=r14,..., frame[14]=rax. vector at frame[15]. */
    /* syscall number in rax = frame[14] */
    u64 num = frame[14];
    /* args: rdi=frame[8]? Let's map: frame[8]=rdi? Count: r15(0),r14(1),r13(2),r12(3),r11(4),r10(5),r9(6),r8(7),rbp(8)? wait rbp pushed after rdi. Let me recount pushes: rax,rbx,rcx,rdx,rsi,rdi,rbp,r8,r9,r10,r11,r12,r13,r14,r15. So last pushed r15 top. Index from top: 0:r15,1:r14,2:r13,3:r12,4:r11,5:r10,6:r9,7:r8,8:rbp,9:rdi,10:rsi,11:rdx,12:rcx,13:rbx,14:rax. */
    u64 rdi = frame[9];
    u64 rsi = frame[10];
    u64 rdx = frame[11];
    (void)rdi;(void)rsi;(void)rdx;
    switch(num){
        case 1: /* write: rdi=fd, rsi=buf, rdx=len */
            if(rdi==1){
                const char *b = (const char*)rsi;
                for(u64 i=0;i<rdx;i++) console_putc(b[i]);
            }
            frame[14] = rdx; /* return bytes */
            break;
        case 2: /* exit */
            console_puts("[syscall] exit\n");
            frame[14]=0;
            break;
        default:
            console_printf("[syscall] unknown %llu\n", (unsigned long long)num);
            frame[14] = (u64)-1;
    }
}
