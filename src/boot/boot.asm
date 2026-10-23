# boot.asm - Multiboot2 entry, 32->64 bit transition
# am-droid OS

.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH, 0  # i386

.section .multiboot2, "a"
.align 8
multiboot_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long multiboot_header_end - multiboot_header
    .long -(MB2_MAGIC + MB2_ARCH + (multiboot_header_end - multiboot_header))

    # Information request tag
    .align 8
    .short 1   # type: info request
    .short 0   # flags
    .long 20   # size
    .long 1    # memory map
    .long 6    # framebuffer
    .long 0    # end

    # Framebuffer request tag: ask GRUB for a 32bpp linear graphics mode.
    # Without this GRUB stays in VGA text mode (80x25, bpp 16) and the GUI
    # has no framebuffer to draw into.
    .align 8
    .short 5   # type: framebuffer
    .short 0   # flags: required
    .long 20   # size
    .long 1280 # width
    .long 800  # height
    .long 32   # depth

    # End tag
    .align 8
    .short 0
    .short 0
    .long 8
multiboot_header_end:

# Stack
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:

# Page tables (identity + higher half)
.align 4096
.global pml4
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096
pt:
    .skip 4096

.section .text
.code32
.global _start
.type _start, @function
_start:
    # EBX = multiboot info pointer, EAX = magic
    cli
    movl %eax, mb_magic
    movl %ebx, mb_info

    # Set up stack
    movl $stack_top, %esp

    # Check CPUID
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    cmpl %ecx, %eax
    je no_cpuid

    # Check long mode support
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb no_longmode
    movl $0x80000001, %eax
    cpuid
    testl $(1<<29), %edx
    jz no_longmode

    # Set up paging: identity map first 2MB (and higher half optional)
    # PML4[0] -> PDPT
    movl $pdpt, %eax
    orl $0x3, %eax
    movl %eax, pml4
    # PML4[511] -> PDPT (higher half, optional) skip for simplicity

    # PDPT[0] -> PD
    movl $pd, %eax
    orl $0x3, %eax
    movl %eax, pdpt

    # PD: 2MB huge pages identity map first 64MB
    movl $pd, %edi
    movl $0, %ecx
2:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax   # present, writable, huge
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $8, %edi
    incl %ecx
    cmpl $32, %ecx
    jl 2b

    # Enable PAE
    movl %cr4, %eax
    orl $(1<<5), %eax
    movl %eax, %cr4

    # Load PML4
    movl $pml4, %eax
    movl %eax, %cr3

    # Enable long mode (EFER.LME)
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1<<8), %eax
    wrmsr

    # Enable paging
    movl %cr0, %eax
    orl $(1<<31), %eax
    movl %eax, %cr0

    # Load 64-bit GDT
    lgdt gdt64_ptr

    # Far jump to 64-bit code
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    # Set up segments
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Stack in 64-bit
    movq $stack_top, %rsp

    # Clear BSS? already zero from GRUB mostly# do quick
    # Call kernel main with multiboot info
    movl mb_magic, %edi
    movl mb_info, %esi
    call kernel_main

    # Halt
    cli
1:
    hlt
    jmp 1b

.code32
no_cpuid:
no_longmode:
    movl $0x0B8000, %edi
    movb $'N', (%edi)
    jmp .

# Multiboot info saved
.section .data
mb_magic: .long 0
mb_info: .long 0

# GDT
.align 16
gdt64:
    .quad 0  # null
    # code segment
    .quad (1<<43) | (1<<44) | (1<<47) | (1<<53)  # code: present, S, exec, long
    # data segment
    .quad (1<<44) | (1<<47) | (1<<41)  # data: present, S, writable
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64
    .long 0
