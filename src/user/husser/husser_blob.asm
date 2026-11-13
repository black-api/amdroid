/* husser_blob.asm — the Husser renderer, embedded flat into the kernel so
 * it is always available, with no disk needed. out/user/husser.bin is the
 * objcopy of the ET_EXEC at 0x40000000; its first byte is the entry point.
 * out/user/husser_defs.inc (generated) defines husser_bss_end: the end of
 * the zero-initialised image, which the kernel maps as zero pages. */
.include "out/user/husser_defs.inc"
.section .rodata
.global husser_blob_start
.global husser_blob_end
husser_blob_start:
.incbin "out/user/husser.bin"
husser_blob_end:
