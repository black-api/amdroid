/* scribe.c — Scribe, the speech-to-text process: a ring-3 program that runs the Thimble
 * Whisper engine (ThimbleLM/engine/src/stt.c) on samples the kernel puts in shared memory.
 * Like Reel it is separate so the engine's floats, big heap and any bug stay out of the kernel.
 * scribe_proto.h has the wire format; src/kernel/scribe_bridge.c is the other side. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "amdroid/scribe_proto.h"
#include "thimble_stt.h"
#include "thimble_needle.h"
#include "amdroid/voice_tools.h"

void scribe_heap_init(void *base, uint64_t bytes);

#define SYS_EXIT  2
#define SYS_YIELD 3
#define SYS_TICKS 5
#define SYS_SLEEP 15
static int64_t sc3(int64_t n, int64_t a, int64_t b, int64_t c){
    int64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory", "rcx", "r11");
    return r;
}
static inline uint64_t rdtsc(void){ uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static uint64_t tsc_per_us = 1000;
static double now_us(void){ return (double)rdtsc() / (double)tsc_per_us; }
static void calibrate(void){
    uint64_t t = (uint64_t)sc3(SYS_TICKS, 0, 0, 0);
    while((uint64_t)sc3(SYS_TICKS, 0, 0, 0) == t) ;
    uint64_t c0 = rdtsc(), t0 = (uint64_t)sc3(SYS_TICKS, 0, 0, 0);
    while((uint64_t)sc3(SYS_TICKS, 0, 0, 0) < t0 + 5) ;
    tsc_per_us = (rdtsc() - c0) / 50000;      /* 5 ticks at 100 Hz = 50 ms */
    if(!tsc_per_us) tsc_per_us = 1;
}

static volatile scribe_hdr_t *const hdr = (scribe_hdr_t *)SCRIBE_SHM_VA;
#define PCM  ((const float *)(SCRIBE_SHM_VA + SCRIBE_PCM_OFF))
#define TEXT ((char *)(SCRIBE_SHM_VA + SCRIBE_TEXT_OFF))
#define CALLS ((char *)(SCRIBE_SHM_VA + SCRIBE_CALLS_OFF))
#define REASON ((char *)(SCRIBE_SHM_VA + SCRIBE_REASON_OFF))

void scribe_fatal(const char *msg){
    { size_t i = 0; for(; msg[i] && i < sizeof hdr->err - 1; i++) hdr->err[i] = msg[i]; hdr->err[i] = 0; }
    hdr->state = SC_ST_FAILED;
    for(int i = 0; i < 10; i++) sc3(SYS_YIELD, 0, 0, 0);
    sc3(SYS_EXIT, 1, 0, 0);
    for(;;){}
}

static thim_needle *needle;

/* Needle 3 from the mapped archive, with Am-Droid's tools run through once (a few seconds:
 * the kernel asks for it as soon as the microphone opens for a command) */
static void needle_load(void){
    if(needle || hdr->needle_state == SC_NEEDLE_FAILED) return;
    if(!hdr->needle_bytes){ hdr->needle_state = SC_NEEDLE_FAILED; return; }
    hdr->needle_state = SC_NEEDLE_LOADING;
    double t0 = now_us();
    char err[160];
    thim_needle *n = thim_needle_open((const void *)SCRIBE_NEEDLE_VA, hdr->needle_bytes, err, sizeof err);
    if(n && thim_needle_set_tools(n, voice_tools_json, voice_system, err, sizeof err) != 0){ thim_needle_close(n); n = 0; }
    if(!n){
        size_t i = 0; for(; err[i] && i < sizeof hdr->err - 1; i++) hdr->err[i] = err[i]; hdr->err[i] = 0;
        hdr->needle_state = SC_NEEDLE_FAILED;
        return;
    }
    needle = n;
    hdr->needle_load_us = (uint32_t)(now_us() - t0);
    hdr->needle_state = SC_NEEDLE_READY;
}

static void command(const char *text){
    CALLS[0] = 0; REASON[0] = 0;
    hdr->n_calls = 0; hdr->withheld = 0; hdr->confidence = hdr->head = hdr->decode = 0;
    needle_load();
    if(!needle){ hdr->withheld = -1; return; }
    thim_needle_opts o = { 0 };
    o.gates = THIM_GATE_ALL;
    thim_needle_result r;
    double t0 = now_us();
    int rc = thim_needle_run(needle, text, &o, CALLS, SCRIBE_CALLS_BYTES, REASON, SCRIBE_REASON_BYTES, &r);
    hdr->needle_us = (uint32_t)(now_us() - t0);
    hdr->needle_tokens = (uint32_t)r.new_tokens;
    if(rc != 0){ CALLS[0] = 0; hdr->withheld = -1; return; }
    hdr->n_calls = (uint32_t)r.n_calls; hdr->withheld = r.withheld;
    hdr->confidence = r.confidence; hdr->head = r.head; hdr->decode = r.decode;
    uint32_t l = 0; while(CALLS[l]) l++;
    hdr->calls_len = l;
}

__attribute__((noreturn)) void scribe_main(void){
    if(hdr->magic != SCRIBE_MAGIC) scribe_fatal("wrong shared-memory magic");
    scribe_heap_init((void *)SCRIBE_HEAP_VA, hdr->heap_bytes);
    calibrate();
    thim_stt_set_clock(now_us);
    thim_needle_set_clock(now_us);
    char err[160];
    thim_stt *s = thim_stt_open((const void *)SCRIBE_MODEL_VA, hdr->model_bytes, err, sizeof err);
    if(!s) scribe_fatal(err);
#if defined(__SSE4_1__)
    hdr->cpu_ok = 1;
#endif
    hdr->state = SC_ST_READY;
    uint32_t last = hdr->req_seq;
    for(;;){
        uint32_t seq = hdr->req_seq;
        if(seq == last){
            if(hdr->want_needle && !needle && hdr->needle_state != SC_NEEDLE_FAILED) needle_load();
            else sc3(SYS_SLEEP, 1, 0, 0);
            continue;
        }
        last = seq;
        uint32_t mode = hdr->mode;
        if(mode == SC_MODE_ASK){
            command(TEXT);
        } else {
            uint32_t n = hdr->n_samples;
            if(n > SCRIBE_MAX_SAMPLES) n = SCRIBE_MAX_SAMPLES;
            int len = thim_stt_transcribe(s, PCM, (int)n, NULL, TEXT, SCRIBE_TEXT_BYTES);
            if(len < 0){ TEXT[0] = 0; len = 0; }
            thim_stt_stats st; thim_stt_stats_get(s, &st);
            hdr->text_len = (uint32_t)len;
            hdr->enc_frames = (uint32_t)st.enc_frames; hdr->tokens = (uint32_t)st.tokens;
            hdr->mel_us = (uint32_t)st.mel_us; hdr->enc_us = (uint32_t)st.encoder_us; hdr->dec_us = (uint32_t)st.decoder_us;
            if(mode == SC_MODE_COMMAND){ if(len > 0) command(TEXT); else { CALLS[0] = 0; hdr->n_calls = 0; hdr->withheld = 0; } }
        }
        __asm__ volatile("" ::: "memory");
        hdr->done_seq = seq;
    }
}

__asm__(".section .text.entry\n.global _start\n_start:\n  andq $-16, %rsp\n  call scribe_main\n  ud2\n.previous");
