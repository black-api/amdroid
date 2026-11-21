/* Reel: a flat image at 0x40000000 (like Husser), entry first. musl and the codec
 * libraries bring init/fini arrays and unwind tables; Reel runs no constructors and never
 * unwinds, so those are dropped. */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text.entry) *(.text*) }
    .rodata : ALIGN(64) { *(.rodata*) }
    .data : ALIGN(64) { *(.data*) *(.init_array*) *(.fini_array*) }
    .bss : ALIGN(64) { *(.bss*) *(COMMON) }
    __reel_end = ALIGN(4096);
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) *(.gnu*) }
}
