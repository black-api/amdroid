/* media.h — the OS's media pipeline, the part that is ours: demuxing the containers
 * browsers get from video sites (fragmented MP4 and WebM), an MSE-style source buffer
 * that holds coded frames by time, and turning decoded pictures into pixels.
 *
 * The decoders themselves are vendored (libavcodec for H.264/VP9/AAC/Opus, dav1d for AV1;
 * see tools/codecs/README.md) and wrapped by codec.c. All of this runs in Reel, the ring-3
 * media process, and also builds on the host for tests (tools/harness/mediatest.c).
 *
 * Times are microseconds (int64) everywhere. */
#ifndef AMDROID_MEDIA_H
#define AMDROID_MEDIA_H
#include <stdint.h>
#include <stddef.h>

/* codecs */
enum { MC_NONE = 0, MC_H264, MC_VP9, MC_AV1, MC_AAC, MC_OPUS };
/* track kinds */
enum { MK_NONE = 0, MK_VIDEO, MK_AUDIO };

const char *media_codec_name(int codec);

/* ---- source buffer: an MSE SourceBuffer without the JavaScript ----
 * Bytes go in with sbuf_append in any pieces (an init segment, then media segments, as
 * the network delivers them). Whole boxes/elements are parsed as they complete; each
 * media segment is kept as one block, and its coded frames point into it. */
#define SB_MAX_TRACKS 4

typedef struct mseg {
    uint8_t *data;
    uint32_t len;
    int refs;                   /* samples still pointing into it */
} mseg_t;

typedef struct msample {
    int64_t pts, dts, dur;      /* microseconds */
    const uint8_t *data;
    uint32_t len;
    uint8_t key;                /* a random access point */
    mseg_t *seg;
} msample_t;

typedef struct mtrack {
    int id;                     /* container track id (mp4) or number (webm) */
    int kind, codec;
    int width, height;          /* video */
    int rate, channels;         /* audio */
    uint8_t *extra;             /* codec config: avcC, AudioSpecificConfig, OpusHead, av1C */
    int extralen;
    uint32_t timescale;         /* mp4: ticks per second */
    int64_t shift;              /* mp4: edit-list start, subtracted from every time (ticks) */
    uint32_t def_dur, def_size, def_flags;   /* mp4 trex defaults */
    int64_t codec_delay;        /* webm: CodecDelay (us), Opus pre-skip */
    msample_t *s;               /* coded frames in decode order */
    int n, cap;
    uint32_t gen;               /* bumped whenever frames are removed or inserted out of order */
} mtrack_t;

typedef struct sbuf {
    int fmt;                    /* 0 unknown, 1 mp4, 2 webm */
    int have_init;              /* an init segment was parsed: tracks[] are known */
    int error;                  /* the stream could not be parsed (sticky until reset) */
    char errmsg[96];
    mtrack_t tracks[SB_MAX_TRACKS];
    int ntracks;
    /* bytes not parsed yet */
    uint8_t *in;
    uint32_t in_len, in_cap;
    /* webm state */
    uint64_t wm_timescale;      /* ns per tick (TimecodeScale) */
    int64_t wm_cluster_tc;      /* the current cluster's timecode */
    int wm_in_segment, wm_in_cluster;
    int64_t duration_us;        /* from the container, or -1 */
    uint64_t bytes_held;        /* coded bytes in segments (for eviction) */
} sbuf_t;

void sbuf_init(sbuf_t *b);
void sbuf_free(sbuf_t *b);
/* drop pending bytes (MSE abort()): a half-received segment is thrown away */
void sbuf_abort(sbuf_t *b);
/* returns 0, or -1 when the stream is broken (b->errmsg says why) */
int  sbuf_append(sbuf_t *b, const uint8_t *data, uint32_t len);
mtrack_t *sbuf_track(sbuf_t *b, int kind);   /* the first track of a kind, or NULL */
/* buffered time ranges of a track: up to max [start,end) pairs; returns how many */
int  sbuf_buffered(const mtrack_t *t, int64_t *ranges, int max);
/* MSE remove(): frames with pts in [start, end) */
void sbuf_remove(sbuf_t *b, int64_t start, int64_t end);
/* index of the frame to start decoding from for time t: the last key frame at or before
 * t (video), or the frame playing at t (audio); -1 when t is not buffered */
int  sbuf_seek_index(const mtrack_t *t, int64_t at);
/* index of the first frame with dts > dts (the next to decode), or n */
int  sbuf_next_index(const mtrack_t *t, int64_t dts);

/* ---- decoders (codec.c) ---- */
typedef struct mpic {
    int w, h;
    const uint8_t *y, *u, *v;
    int ys, uvs;                /* strides */
    int cw, ch;                 /* chroma size */
    int64_t pts;
    int bt709;                  /* colour matrix: 1 BT.709, 0 BT.601 */
    int full_range;
} mpic_t;

typedef struct mdec mdec_t;
mdec_t *mdec_open(const mtrack_t *t, char *err, int errcap);
void    mdec_close(mdec_t *d);
void    mdec_flush(mdec_t *d);                  /* before a seek */
/* 0 accepted, 1 full (take output first), -1 error */
int     mdec_send(mdec_t *d, const msample_t *s);
/* video: 1 and *pic filled (valid until the next call), 0 none ready, -1 error */
int     mdec_picture(mdec_t *d, mpic_t *pic);
/* audio: decoded and resampled to 48 kHz 16-bit stereo; returns frames written to out
 * (at most max), 0 when none are ready; *pts is the time of the first one */
int     mdec_audio(mdec_t *d, int16_t *out, int max, int64_t *pts);
/* send end-of-stream so the decoder hands over the frames it holds back */
void    mdec_drain(mdec_t *d);
void    mdec_skip_nonref(mdec_t *d, int on);

/* ---- pixels (yuv.c) ---- */
/* Converts a picture to XRGB8888 at dw x dh (any size: scaled), rows dstride pixels apart */
void yuv_to_rgb(const mpic_t *p, uint32_t *dst, int dw, int dh, int dstride);
/* the same; smooth: bilinear instead of nearest when the size changes (slower) */
void yuv_to_rgb_q(const mpic_t *p, uint32_t *dst, int dw, int dh, int dstride, int smooth);

#endif
