# hello.asm — first program that runs in ring 3. Position independent: the
# kernel copies this blob to VM_USER_BASE and starts it there.
.section .text
.global user_hello_start
user_hello_start:
    movq $1, %rax               # SYS_WRITE
    movq $1, %rdi               # fd 1 = console
    leaq msg(%rip), %rsi
    movq $(msg_end - msg), %rdx
    int $0x80
    movq $2, %rax               # SYS_EXIT
    movq $7, %rdi               # exit code 7
    int $0x80
spin:
    jmp spin
msg:
    .ascii "hello from ring 3\n"
msg_end:
.global user_hello_end
user_hello_end:
