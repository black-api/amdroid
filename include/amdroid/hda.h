#ifndef AMDROID_HDA_H
#define AMDROID_HDA_H
#include "amdroid/types.h"
#include <stdint.h>
int hda_init(void);            /* finds and starts an Intel HD Audio controller; 0 on success */
int hda_play_tone(int hz, int ms);   /* plays a sine tone (48 kHz, 16-bit stereo) */
int hda_record(int ms, int16_t *out, u32 *frames);   /* captures audio: out holds ms*192 bytes */
#endif
