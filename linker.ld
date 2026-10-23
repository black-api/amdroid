ENTRY(_start)

SECTIONS
{
    . = 0x100000;
    __kernel_start = .;

    .multiboot2 : {
        *(.multiboot2)
    }

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
