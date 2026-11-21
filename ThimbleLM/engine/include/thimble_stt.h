/*
 * Thimble engine - speech to text.
 *
 * Runs a Whisper-style encoder/decoder from a .thim file (see tools/thim_convert.py). The
 * file is read in place: give thim_stt_open() the bytes (a mapped file, or a blob linked into
 * the program) and keep them alive while the context exists. No other file, no libm, no
 * threads. All memory comes from the allocator hooks below (malloc/free by default).
 */
#ifndef THIMBLE_STT_H
#define THIMBLE_STT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thim_stt thim_stt;

/* Memory hooks: call before thim_stt_open to use your own allocator. */
void thim_stt_set_alloc(void *(*alloc)(size_t), void (*release)(void *));

/* The model must stay in memory (and 8-byte aligned) until thim_stt_close. NULL + message on failure. */
thim_stt *thim_stt_open(const void *thim, size_t len, char *err, size_t err_len);
void      thim_stt_close(thim_stt *s);

int thim_stt_sample_rate(const thim_stt *s);   /* 16000 */

typedef struct {
    int   max_tokens;      /* stop after this many new tokens (0: 224) */
    int   min_ctx;         /* least encoder frames (50 per second of audio; 0: 100) */
    int   pad_ctx;         /* extra frames after the audio, for the model's sense of an ending (0: 25) */
    int   full_ctx;        /* nonzero: always a whole 30 s window (slow, the reference behaviour) */
} thim_stt_opts;

/* Transcribe mono 16 kHz float samples in [-1, 1]. Writes UTF-8 text (leading space trimmed) to
 * out and returns its length, or -1 on error. */
int thim_stt_transcribe(thim_stt *s, const float *pcm, int n_samples, const thim_stt_opts *opts, char *out, size_t out_cap);

/* Timing of the last call, in microseconds (0 where the host gave no clock). */
typedef struct { double mel_us, encoder_us, decoder_us; int enc_frames, tokens; } thim_stt_stats;
void thim_stt_stats_get(const thim_stt *s, thim_stt_stats *st);
void thim_stt_set_clock(double (*now_us)(void));

#ifdef __cplusplus
}
#endif
#endif
