#ifndef AMDROID_SCRIBE_H
#define AMDROID_SCRIBE_H
/* The kernel side of Scribe, the speech-to-text process (scribe_proto.h has the format). */
#include "amdroid/types.h"
#include "amdroid/scribe_proto.h"

/* Transcribes mono 16 kHz samples in [-1, 1] (at most SCRIBE_MAX_SAMPLES; longer is cut).
 * Starts Scribe on first use and waits (yielding) for the answer. Writes NUL-terminated
 * UTF-8 into out; returns its length, or -1 with a reason in out. */
int  scribe_transcribe(const float *pcm, u32 n, char *out, u32 cap);
/* The same from a WAV file's bytes (PCM 8/16/32-bit int or 32-bit float, any channels/rate). */
int  scribe_transcribe_wav(const u8 *wav, u32 len, char *out, u32 cap);
/* Without waiting (for the GUI): submit copies the samples and returns 0 (or -1, reason in why);
 * poll returns -2 while Scribe works, then the text length (text in out) or -1 (reason in out). */
int  scribe_submit(const float *pcm, u32 n, char *why, u32 cap);
int  scribe_poll(char *out, u32 cap);
int  scribe_busy(void);
/* Voice commands. prepare: start Scribe and have it load Needle now (call when the mic opens).
 * submit_command: audio -> text -> a call; submit_ask: text -> a call. Both finish through
 * scribe_poll (which then returns the length of what was heard); the call is in the result. */
typedef struct {
    char heard[512];
    char calls[1024];         /* [{"name":..,"arguments":{..}},..], grammar-made */
    char reasoning[256];
    float confidence;
    int withheld;             /* 0 ok, else THIM_WITHHELD_* (1 confidence 2 negation 3 number 4 text 5 enum), -1 error */
    int n_calls;
    u32 needle_ms, load_ms, stt_ms;
} scribe_cmd_t;
int  scribe_prepare_commands(char *why, u32 cap);
int  scribe_submit_command(const float *pcm, u32 n, char *why, u32 cap);
int  scribe_submit_ask(const char *text, char *why, u32 cap);
void scribe_command_result(scribe_cmd_t *r);
const volatile scribe_hdr_t *scribe_header(void);
void scribe_shutdown(void);
#endif
