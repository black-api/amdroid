# isr_stubs.asm
.section .text
.code64

.macro ISR_NOERR num
.global isr_\num
isr_\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr_\num
isr_\num:
    pushq $\num
    jmp isr_common
.endm

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    # Save the SSE/x87 state of the interrupted task into its fxsave area.
    movq cur_fx_ptr(%rip), %r12
    fxsave (%r12)
    movq %rsp, %rdi            # frame: argument for isr_handler
    andq $-16, %rsp            # C code expects a 16-byte aligned stack
    call isr_handler           # returns the frame to resume (maybe another task's)
    movq %rax, %rsp
    # Restore the SSE state of the task we are resuming.
    movq cur_fx_ptr(%rip), %r12
    fxrstor (%r12)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp             # drop vector and error code
    iretq

# Generate stubs for 0..31 (exceptions) and 32..47 (IRQs)
# We'll generate via macro calls
.altmacro

# exceptions 0-31
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR 30
ISR_NOERR 31

# IRQs 32-47
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

# syscall (int 0x80) and yield (int 0x81)
ISR_NOERR 128
ISR_NOERR 129

