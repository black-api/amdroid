#ifndef AMDROID_SCRIBE_PROTO_H
#define AMDROID_SCRIBE_PROTO_H
/* The wire format between the kernel and Scribe, the speech-to-text process.
 *
 * The kernel copies 16 kHz mono float samples into the region, bumps req_seq and yields;
 * Scribe transcribes them and writes UTF-8 into the text area, then sets done_seq = req_seq.
 * Clients (the microphone, apps) talk to scribe_bridge.c, never to Scribe directly.
 * The model is not in the region: it is mapped read-only from the kernel image at SCRIBE_MODEL_VA. */
#include <stdint.h>

#define SCRIBE_SHM_VA      0x60000000ULL
#define SCRIBE_HEAP_VA     0x48000000ULL
#define SCRIBE_HEAP_BYTES  (128u * 1024 * 1024)   /* up to SCRIBE_MODEL_VA */
#define SCRIBE_MODEL_VA    0x50000000ULL     /* Whisper: the .thim file, page aligned */
#define SCRIBE_NEEDLE_VA   0x54000000ULL     /* Needle 3: needle3.cact, page aligned */
#define SCRIBE_RATE        16000u
#define SCRIBE_MAX_SECONDS 30u
#define SCRIBE_MAX_SAMPLES (SCRIBE_RATE * SCRIBE_MAX_SECONDS)

#define SCRIBE_MAGIC       0x42524353u       /* 'SCRB' */
#define SCRIBE_HDR_BYTES   4096u
#define SCRIBE_PCM_OFF     SCRIBE_HDR_BYTES
#define SCRIBE_PCM_BYTES   (SCRIBE_MAX_SAMPLES * 4u)
#define SCRIBE_TEXT_OFF    (SCRIBE_PCM_OFF + SCRIBE_PCM_BYTES)
#define SCRIBE_TEXT_BYTES  4096u
#define SCRIBE_CALLS_OFF   (SCRIBE_TEXT_OFF + SCRIBE_TEXT_BYTES)
#define SCRIBE_CALLS_BYTES 2048u             /* the call list (JSON), then the reasoning line */
#define SCRIBE_REASON_OFF  (SCRIBE_CALLS_OFF + SCRIBE_CALLS_BYTES)
#define SCRIBE_REASON_BYTES 2048u
#define SCRIBE_SHM_BYTES   (SCRIBE_REASON_OFF + SCRIBE_REASON_BYTES)

/* request modes */
#define SC_MODE_TRANSCRIBE 0                 /* audio -> text */
#define SC_MODE_COMMAND    1                 /* audio -> text -> a tool call (Needle) */
#define SC_MODE_ASK        2                 /* text (in the text area) -> a tool call */

/* Needle, written by Scribe */
#define SC_NEEDLE_NONE     0
#define SC_NEEDLE_LOADING  1
#define SC_NEEDLE_READY    2
#define SC_NEEDLE_FAILED   3

/* states, written by Scribe */
#define SC_ST_BOOT    0
#define SC_ST_READY   1
#define SC_ST_FAILED  2     /* the model would not open: err holds why */

typedef struct {
    uint32_t magic;
    volatile uint32_t state;
    volatile uint32_t req_seq;      /* the kernel asks: bumped by the kernel */
    volatile uint32_t done_seq;     /* Scribe answers: equals req_seq when the text is ready */
    uint32_t n_samples;             /* of the request */
    uint32_t text_len;              /* of the answer */
    uint32_t model_bytes;           /* the mapped model's length */
    uint32_t heap_bytes;
    uint32_t enc_frames, tokens;    /* stats of the last answer */
    uint32_t mel_us, enc_us, dec_us;
    uint32_t cpu_ok;                /* 1: the engine's SSE4.1 path ran */
    char err[160];
    /* voice commands */
    uint32_t needle_bytes;          /* the mapped needle3.cact's length (0: none) */
    volatile uint32_t mode;         /* SC_MODE_* of the request */
    volatile uint32_t want_needle;  /* the kernel asks Scribe to get Needle ready (while the mic records) */
    volatile uint32_t needle_state; /* SC_NEEDLE_* */
    float confidence, head, decode; /* of the last call */
    int32_t withheld;               /* THIM_OK or THIM_WITHHELD_* */
    uint32_t n_calls, calls_len;
    uint32_t needle_load_us, needle_us, needle_tokens;
} scribe_hdr_t;

#endif
