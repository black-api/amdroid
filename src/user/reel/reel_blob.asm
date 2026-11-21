/* reel_blob.asm — Reel, the media process, embedded flat into the kernel like Husser.
 * out/user/reel.bin is the objcopy of the ET_EXEC at 0x40000000; out/user/reel_defs.inc
 * (generated) gives the entry point and the end of its zero-initialised data. */
.include "out/user/reel_defs.inc"
.section .rodata
.global reel_blob_start
.global reel_blob_end
.balign 4096
reel_blob_start:
.incbin "out/user/reel.bin"
reel_blob_end:
