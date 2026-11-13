#ifndef AMDROID_HDA_H
#define AMDROID_HDA_H
#include "amdroid/types.h"
#include <stdint.h>
int hda_init(void);            /* finds and starts an Intel HD Audio controller; 0 on success */
int hda_play_tone(int hz, int ms);   /* plays a sine tone (48 kHz, 16-bit stereo) */
int hda_record(int ms, int16_t *out, u32 *frames);   /* captures audio: out holds ms*192 bytes */
/* streaming playback (48 kHz, 16-bit, stereo): start, keep writing, stop */
int hda_present(void);                          /* 1 if there is a sound card */
int hda_stream_start(void);
u32 hda_stream_write(const void *pcm, u32 bytes);   /* returns the bytes that fit */
u32 hda_stream_room(void);
u64 hda_stream_played(void);                     /* bytes played so far (the audio clock) */
void hda_stream_stop(void);
#endif
