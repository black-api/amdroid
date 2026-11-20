#ifndef AMDROID_REEL_PROTO_H
/* The wire format between the kernel and Reel, the ring-3 media process.
 *
 * Reel owns everything between coded bytes and pixels/PCM: the source buffers (MSE),
 * demuxing, the decoders and colour conversion. The kernel owns time: it feeds Reel's PCM
 * to the sound card, keeps the clock (from what the card has played) and puts the right
 * picture on screen. Clients (the browser's <video>, MVP) talk to the kernel side
 * (reel_bridge.c), never to Reel directly.
 *
 * The region is mapped into Reel at REEL_SHM_VA; the kernel reaches it page by page.
 * Compiled by both the kernel (freestanding) and Reel (musl), so plain fixed-size types. */
#define AMDROID_REEL_PROTO_H
#include <stdint.h>

#define REEL_SHM_VA        0x60000000ULL
#define REEL_HEAP_VA       0x48000000ULL
#define REEL_HEAP_BYTES    (192u * 1024 * 1024)
#define REEL_HEAP_MIN      (96u * 1024 * 1024)

#define REEL_MAGIC         0x4C454552u        /* 'REEL' */
#define REEL_VER           1u

#define REEL_HDR_BYTES     16384u
#define REEL_IN_OFF        REEL_HDR_BYTES
#define REEL_IN_BYTES      (4u * 1024 * 1024)        /* one append at a time */
#define REEL_PCM_OFF       (REEL_IN_OFF + REEL_IN_BYTES)
#define REEL_PCM_FRAMES    (65536u * 2)              /* 2.7 s of 48 kHz stereo s16 (4 bytes a frame) */
#define REEL_PCM_BYTES     (REEL_PCM_FRAMES * 4u)
#define REEL_FRAME_OFF     (REEL_PCM_OFF + REEL_PCM_BYTES)
#define REEL_MAX_W         1920
#define REEL_MAX_H         1080
#define REEL_FRAME_BYTES   ((uint32_t)REEL_MAX_W * REEL_MAX_H * 4)
#define REEL_SLOTS         3
#define REEL_SHM_BYTES     (REEL_FRAME_OFF + (uint64_t)REEL_SLOTS * REEL_FRAME_BYTES)

#define REEL_SBS           2                  /* source buffers (MSE allows more; YouTube uses 2) */
#define REEL_RANGES        8

/* commands (kernel -> reel) */
#define R_CMD_NONE     0
#define R_CMD_RESET    1   /* a new presentation: everything is dropped */
#define R_CMD_APPEND   2   /* a = source buffer, b = bytes at REEL_IN_OFF */
#define R_CMD_REMOVE   3   /* a = source buffer, t0..t1 */
#define R_CMD_ABORT    4   /* a = source buffer: drop a half-parsed segment */
#define R_CMD_SEEK     5   /* t0 = time: decoders restart from the key frame before it */
#define R_CMD_SIZE     6   /* a x b = picture size wanted; smooth = bilinear */
#define R_CMD_EOS      7   /* no more data will come: hand out what the decoders hold */
#define R_CMD_REDRAW   8   /* convert the last picture again (new size or smoothing) */
#define R_CMD_QUIT     9

/* states */
#define R_ST_BOOT      0
#define R_ST_READY     1
#define R_ST_FAILED    2

/* frame slot states */
#define R_SLOT_FREE    0
#define R_SLOT_READY   1   /* reel filled it; the kernel shows it when its time comes */
#define R_SLOT_SHOWN   2   /* on screen now; freed when the next one replaces it */

typedef struct reel_sb_status {
    uint32_t used;                 /* a client appended to it */
    uint32_t have_init;
    uint32_t error;
    uint32_t kind;                 /* MK_* of its first track */
    uint32_t codec;                /* MC_* */
    uint32_t width, height, rate, channels;
    uint32_t nranges;
    int64_t  ranges[REEL_RANGES * 2];
    uint64_t bytes_held;
} reel_sb_status_t;

typedef struct reel_slot {
    volatile uint32_t state;
    uint32_t w, h;                 /* the picture in this slot (rows are w pixels apart) */
    uint32_t seq;
    int64_t  pts;
} reel_slot_t;

typedef struct reel_shm {
    uint32_t magic, ver;
    uint32_t heap_bytes;

    /* command channel */
    volatile uint32_t cmd, cmd_seq, ack_seq;
    int32_t a, b;
    int64_t t0, t1;
    uint32_t smooth;

    /* reel -> kernel */
    volatile uint32_t state;
    char     error[128];           /* the last thing that went wrong, for the user */
    volatile uint32_t error_seq;
    char     log[160];             /* a line for the serial log (the bridge prints it) */
    volatile uint32_t log_seq;
    uint32_t heap_used_mb, heap_peak_mb;
    reel_sb_status_t sb[REEL_SBS];
    int64_t  duration_us;          /* from the containers, -1 unknown */
    uint32_t has_video, has_audio;
    uint32_t video_w, video_h;     /* the coded size */
    uint32_t want_w, want_h;       /* the size pictures are converted to */
    uint64_t decoded, dropped, converted;
    uint64_t decode_us;            /* time spent decoding video (rdtsc-based estimate) */
    volatile uint32_t ended;       /* EOS given and every picture/sample has been handed over */

    /* the clock (kernel -> reel): where playback is, so Reel can drop late pictures and
     * forget data that is behind */
    volatile int64_t clock_us;
    volatile uint32_t playing;

    /* audio ring (reel writes, kernel reads): frames of 48 kHz stereo s16. Position P in the
     * ring is time pcm_t0 + (P - pcm_p0) / 48000; Reel keeps it continuous (silence for a
     * gap) and resets t0/p0 on a seek (pcm_epoch changes). */
    volatile uint64_t pcm_w;       /* frames written (total) */
    volatile uint64_t pcm_r;       /* frames the kernel is done with: Reel may overwrite below pcm_r */
    volatile int64_t  pcm_t0;
    volatile uint64_t pcm_p0;
    volatile uint32_t pcm_epoch;   /* bumped on each seek/reset */

    reel_slot_t slot[REEL_SLOTS];
    volatile uint32_t slot_epoch;  /* bumped on seek/reset: slots from before are stale */
} reel_shm_t;

#endif
