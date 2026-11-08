# busy.asm — preemption test. rdi = a character. Prints it 5 times with a
# busy wait in between, then exits with code 1. Two copies running at once
# interleave their output only if the kernel preempts them.
.section .text
.global user_busy_start
user_busy_start:
    movq %rdi, %rbx             # keep the character in rbx (preserved by syscalls)
    movq $5, %r12
1:
    subq $8, %rsp
    movq %rbx, (%rsp)
    movq $1, %rax               # SYS_WRITE
    movq $1, %rdi
    movq %rsp, %rsi
    movq $1, %rdx
    int $0x80
    addq $8, %rsp
    movq $3000000, %rcx
2:  decq %rcx
    jnz 2b
    decq %r12
    jnz 1b
    movq $2, %rax               # SYS_EXIT
    movq $1, %rdi
    int $0x80
spin:
    jmp spin
.global user_busy_end
user_busy_end:

# fault.asm — reads kernel memory from ring 3. The kernel must kill it.
.section .text
.global user_fault_start
user_fault_start:
    movq 0x100000, %rax
    movq $2, %rax               # not reached
    movq $9, %rdi
    int $0x80
spin2:
    jmp spin2
.global user_fault_end
user_fault_end:
