/* scribe_blob.asm — Scribe and its model, embedded in the kernel like Reel.
 * The model is page aligned so the bridge can map its pages read-only into Scribe. */
.include "out/user/scribe_defs.inc"
.section .rodata
.global scribe_blob_start
.global scribe_blob_end
.global scribe_model_start
.global scribe_model_end
.global scribe_needle_start
.global scribe_needle_end
.balign 4096
scribe_blob_start:
.incbin "out/user/scribe.bin"
scribe_blob_end:
.balign 4096
scribe_model_start:
.incbin "ThimbleLM/models/whisper-tiny-en.thim"
scribe_model_end:
.balign 4096
scribe_needle_start:
.incbin "ThimbleLM/hf/needle3/needle3.cact"
scribe_needle_end:
