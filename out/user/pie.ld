ENTRY(_start)
SECTIONS
{
    . = 0x1000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data.rel.ro : { *(.data.rel.ro*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    .dynamic : { *(.dynamic) }
    .rela.dyn : { *(.rela.dyn) }
}
