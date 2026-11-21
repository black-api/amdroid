/* reel.c — Reel, the media process: a ring-3 program that turns coded media into pictures
 * and sound. The kernel hands it bytes (what a page appended to its MSE source buffers, or
 * what MVP read from a file); Reel demuxes them (src/media/sbuf.c), decodes them (the
 * vendored libavcodec and dav1d, through src/media/codec.c), converts pictures to pixels
 * (src/media/yuv.c) and leaves PCM and pictures in shared memory. The kernel plays the PCM,
 * keeps the clock and shows each picture when its time comes (src/kernel/reel_bridge.c).
 *
 * Like Husser it is a separate process so a broken stream or a decoder bug kills only it:
 * the browser keeps its page and says the video failed. */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "amdroid/reel_proto.h"
#include "media.h"
#include "libavutil/log.h"

void reel_heap_init(void *base, uint64_t bytes);
uint64_t reel_heap_used(void);
uint64_t reel_heap_peak(void);

/* ---- system calls ---- */
#define SYS_EXIT  2
#define SYS_YIELD 3
#define SYS_TICKS 5
#define SYS_SLEEP 15
static int64_t sc3(int64_t n, int64_t a, int64_t b, int64_t c){
    int64_t r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory", "rcx", "r11");
    return r;
}
static uint64_t ticks(void){ return (uint64_t)sc3(SYS_TICKS, 0, 0, 0); }
static void nap(void){ sc3(SYS_SLEEP, 1, 0, 0); }
static inline uint64_t rdtsc(void){ uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static uint64_t tsc_per_us = 1000;

static volatile reel_shm_t *const shm = (reel_shm_t *)REEL_SHM_VA;
#define IN_AREA ((const uint8_t *)(REEL_SHM_VA + REEL_IN_OFF))
#define PCM_RING ((int16_t *)(REEL_SHM_VA + REEL_PCM_OFF))
static uint32_t *slot_px(int k){ return (uint32_t *)(REEL_SHM_VA + REEL_FRAME_OFF + (uint64_t)k * REEL_FRAME_BYTES); }

/* ---- messages ---- */
static void say(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf((char *)shm->log, sizeof shm->log, fmt, ap);
    va_end(ap);
    shm->log_seq++;
    /* give the bridge a moment to print it before the next line replaces it */
    for(int i = 0; i < 3; i++) sc3(SYS_YIELD, 0, 0, 0);
}
static void error(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf((char *)shm->error, sizeof shm->error, fmt, ap);
    va_end(ap);
    shm->error_seq++;
}
void reel_fatal(const char *msg){
    error("media process stopped: %s", msg);
    shm->state = R_ST_FAILED;
    for(int i = 0; i < 10; i++) sc3(SYS_YIELD, 0, 0, 0);
    sc3(SYS_EXIT, 1, 0, 0);
    for(;;){}
}
static void av_log_cb(void *avcl, int level, const char *fmt, va_list vl){
    (void)avcl;
    if(level > AV_LOG_ERROR) return;
    char line[160];
    vsnprintf(line, sizeof line, fmt, vl);
    size_t n = strlen(line);
    while(n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    say("decoder: %s", line);
}

/* ---- the presentation ---- */
static sbuf_t sb[REEL_SBS];
static mtrack_t *vt, *at;              /* the tracks being played */
static mdec_t *vd, *ad;
static int vt_codec, at_codec;
static int64_t pos_us;                 /* the last seek target */
static int v_fresh, a_fresh;           /* start from the seek point */
static int64_t v_last_dts, a_last_dts; /* the last frame sent to each decoder */
static int eos, v_drained, a_drained, v_done, a_done;
static uint32_t want_w, want_h;
static int smooth;
static int shown_since_seek;           /* pictures handed over since the last seek */
static int skipping;                   /* decoder told to skip non-reference frames */
static uint32_t slot_seq;

/* copies of the pictures in the slots (for a redraw at another size while paused) */
typedef struct { uint8_t *buf; size_t cap; mpic_t pic; int valid; } keep_t;
static keep_t keep[REEL_SLOTS];

static void close_decoders(void){
    if(vd) mdec_close(vd);
    if(ad) mdec_close(ad);
    vd = ad = NULL;
    vt = at = NULL;
    vt_codec = at_codec = 0;
    skipping = 0;
}

static void free_ready_slots(void){
    for(int k = 0; k < REEL_SLOTS; k++)
        if(shm->slot[k].state == R_SLOT_READY) shm->slot[k].state = R_SLOT_FREE;
}

static void audio_epoch(void){
    shm->pcm_p0 = shm->pcm_w;
    shm->pcm_t0 = pos_us;
    __asm__ volatile("" ::: "memory");
    shm->pcm_epoch++;
}

static void reset_all(void){
    close_decoders();
    for(int i = 0; i < REEL_SBS; i++){ sbuf_free(&sb[i]); sbuf_init(&sb[i]); }
    memset((void *)shm->sb, 0, sizeof shm->sb);
    pos_us = 0;
    v_fresh = a_fresh = 1;
    v_last_dts = a_last_dts = 0;
    eos = v_drained = a_drained = v_done = a_done = 0;
    shown_since_seek = 0;
    shm->ended = 0;
    shm->duration_us = -1;
    shm->has_video = shm->has_audio = 0;
    shm->video_w = shm->video_h = 0;
    shm->decoded = shm->dropped = shm->converted = shm->decode_us = 0;
    free_ready_slots();
    for(int k = 0; k < REEL_SLOTS; k++) keep[k].valid = 0;
    shm->slot_epoch++;
    audio_epoch();
}

static void publish_status(void){
    int64_t dur = -1;
    for(int i = 0; i < REEL_SBS; i++){
        volatile reel_sb_status_t *st = &shm->sb[i];
        sbuf_t *b = &sb[i];
        st->have_init = (uint32_t)b->have_init;
        st->error = (uint32_t)b->error;
        st->bytes_held = b->bytes_held;
        mtrack_t *t = b->ntracks ? &b->tracks[0] : NULL;
        for(int k = 0; k < b->ntracks; k++) if(b->tracks[k].codec){ t = &b->tracks[k]; break; }
        if(t){
            st->kind = (uint32_t)t->kind; st->codec = (uint32_t)t->codec;
            st->width = (uint32_t)t->width; st->height = (uint32_t)t->height;
            st->rate = (uint32_t)t->rate; st->channels = (uint32_t)t->channels;
            /* buffered: the intersection over this buffer's tracks is what MSE reports;
               one track per buffer is the usual case, so the first one stands for it */
            int64_t r[REEL_RANGES * 2];
            int n = sbuf_buffered(t, r, REEL_RANGES);
            for(int k = 0; k < n * 2; k++) st->ranges[k] = r[k];
            st->nranges = (uint32_t)n;
        } else st->nranges = 0;
        if(b->duration_us > dur) dur = b->duration_us;
    }
    shm->duration_us = dur;
    shm->has_video = vt != NULL;
    shm->has_audio = at != NULL;
    if(vt){ shm->video_w = (uint32_t)vt->width; shm->video_h = (uint32_t)vt->height; }
    shm->heap_used_mb = (uint32_t)(reel_heap_used() >> 20);
    shm->heap_peak_mb = (uint32_t)(reel_heap_peak() >> 20);
}

/* finds (or changes to) the tracks to play after an append */
static void pick_tracks(void){
    char err[128];
    for(int i = 0; i < REEL_SBS; i++){
        mtrack_t *v = sbuf_track(&sb[i], MK_VIDEO), *a = sbuf_track(&sb[i], MK_AUDIO);
        if(v && (!vt || vt == v) && (!vd || vt_codec != v->codec)){
            if(vd){ mdec_close(vd); vd = NULL; }
            vd = mdec_open(v, err, sizeof err);
            if(!vd){ error("video: %s", err); say("video: %s", err); }
            else {
                vt = v; vt_codec = v->codec; v_fresh = 1;
                say("video: %s %dx%d", media_codec_name(v->codec), v->width, v->height);
            }
        }
        if(a && (!at || at == a) && (!ad || at_codec != a->codec)){
            if(ad){ mdec_close(ad); ad = NULL; }
            ad = mdec_open(a, err, sizeof err);
            if(!ad){ error("audio: %s", err); say("audio: %s", err); }
            else {
                at = a; at_codec = a->codec; a_fresh = 1;
                say("audio: %s %d Hz, %d channels", media_codec_name(a->codec), a->rate, a->channels);
            }
        }
    }
}

/* MSE's coded frame eviction: keep at most this much behind the playhead */
#define KEEP_BEHIND_US 8000000
static void evict(void){
    /* the bridge sets the clock to the target before it asks for a seek, so this is
       where playback is (or is about to be) */
    int64_t cut = shm->clock_us - KEEP_BEHIND_US;
    if(cut <= 0) return;
    /* nothing the decoders still need: video keeps the GOP being decoded */
    if(vt && !v_fresh && v_last_dts - 1000000 < cut) cut = v_last_dts - 1000000;
    if(at && !a_fresh && a_last_dts - 1000000 < cut) cut = a_last_dts - 1000000;
    if(cut <= 0) return;
    for(int i = 0; i < REEL_SBS; i++) if(sb[i].ntracks) sbuf_remove(&sb[i], -((int64_t)1 << 60), cut);
}

static void do_seek(int64_t t){
    pos_us = t < 0 ? 0 : t;
    if(vd) mdec_flush(vd);
    if(ad) mdec_flush(ad);
    v_fresh = a_fresh = 1;
    v_drained = a_drained = v_done = a_done = 0;
    shm->ended = 0;
    shown_since_seek = 0;
    if(skipping && vd){ mdec_skip_nonref(vd, 0); skipping = 0; }
    free_ready_slots();
    shm->slot_epoch++;
    audio_epoch();
}

/* ---- video ---- */
static int free_slot(void){
    for(int k = 0; k < REEL_SLOTS; k++) if(shm->slot[k].state == R_SLOT_FREE) return k;
    return -1;
}

static void out_size(const mpic_t *p, uint32_t *w, uint32_t *h){
    uint32_t ww = want_w ? want_w : (uint32_t)p->w, hh = want_h ? want_h : (uint32_t)p->h;
    if(ww > REEL_MAX_W) ww = REEL_MAX_W;
    if(hh > REEL_MAX_H) hh = REEL_MAX_H;
    if(!ww) ww = 1;
    if(!hh) hh = 1;
    *w = ww; *h = hh;
}

static void keep_copy(int k, const mpic_t *p){
    keep_t *c = &keep[k];
    c->valid = 0;
    if((size_t)p->w * p->h > 1280u * 720) return;      /* big pictures: no redraw copy */
    size_t ysz = (size_t)p->w * p->h, csz = (size_t)p->cw * p->ch;
    size_t need = ysz + 2 * csz;
    if(need > c->cap){
        free(c->buf);
        c->buf = malloc(need);
        c->cap = c->buf ? need : 0;
        if(!c->buf) return;
    }
    for(int y = 0; y < p->h; y++) memcpy(c->buf + (size_t)y * p->w, p->y + (size_t)y * p->ys, (size_t)p->w);
    for(int y = 0; y < p->ch; y++){
        memcpy(c->buf + ysz + (size_t)y * p->cw, p->u + (size_t)y * p->uvs, (size_t)p->cw);
        memcpy(c->buf + ysz + csz + (size_t)y * p->cw, p->v + (size_t)y * p->uvs, (size_t)p->cw);
    }
    c->pic = *p;
    c->pic.y = c->buf; c->pic.u = c->buf + ysz; c->pic.v = c->buf + ysz + csz;
    c->pic.ys = p->w; c->pic.uvs = p->cw;
    c->valid = 1;
}

static uint64_t convert_us;
static void fill_slot(int k, const mpic_t *p){
    uint32_t w, h;
    out_size(p, &w, &h);
    uint64_t c0 = rdtsc();
    yuv_to_rgb_q(p, slot_px(k), (int)w, (int)h, (int)w, smooth);
    convert_us += (rdtsc() - c0) / tsc_per_us;
    volatile reel_slot_t *s = &shm->slot[k];
    s->w = w; s->h = h;
    s->pts = p->pts;
    s->seq = ++slot_seq;
    __asm__ volatile("" ::: "memory");
    s->state = R_SLOT_READY;
    shm->converted++;
    shown_since_seek++;
}

/* a decoded picture: show it, or drop it when its time is gone */
static void take_picture(const mpic_t *p){
    shm->decoded++;
    if(p->pts < pos_us - 1000 && shown_since_seek == 0){ return; }      /* before the seek point */
    int64_t clock = shm->clock_us;
    int64_t late = clock - p->pts;
    if(shm->playing && shown_since_seek > 0 && late > 30000){
        shm->dropped++;
        /* far behind: let the decoder skip what it can until it catches up */
        if(late > 250000 && !skipping && vd){ mdec_skip_nonref(vd, 1); skipping = 1; }
        return;
    }
    if(skipping && late < 50000 && vd){ mdec_skip_nonref(vd, 0); skipping = 0; }
    int k = free_slot();
    if(k < 0) return;                  /* cannot happen: callers check first */
    keep_copy(k, p);
    fill_slot(k, p);
}

static int video_step(void){
    if(!vd || !vt || v_done) return 0;
    if(free_slot() < 0) return 0;
    uint64_t t0 = rdtsc();
    int did = 0;
    /* pictures the decoder already has come out first */
    for(;;){
        mpic_t p;
        int r = mdec_picture(vd, &p);
        if(r < 0){ say("video: a picture failed to decode"); break; }
        if(r == 0) break;
        take_picture(&p);
        did = 1;
        if(free_slot() < 0) goto out;
    }
    if(v_drained){ v_done = 1; goto out; }
    /* then one coded frame goes in */
    int idx;
    if(v_fresh){
        idx = sbuf_seek_index(vt, pos_us);
        if(idx < 0) goto out;          /* the seek point is not buffered yet */
        v_fresh = 0;
    } else idx = sbuf_next_index(vt, v_last_dts);
    if(idx >= vt->n){
        if(eos && !v_drained){ mdec_drain(vd); v_drained = 1; did = 1; }
        goto out;
    }
    msample_t s = vt->s[idx];
    int r = mdec_send(vd, &s);
    if(r == 1) goto out;               /* full: its pictures first, next time round */
    if(r < 0) say("video: a frame was rejected at %lld ms", (long long)(s.pts / 1000));
    v_last_dts = s.dts;
    did = 1;
out:
    if(did) shm->decode_us += (rdtsc() - t0) / tsc_per_us;
    return did;
}

/* every 5 s while something plays: what the time goes on (the serial log) */
static void stats(void){
    static uint64_t last_t, last_dec, last_conv, last_drop, last_dus, last_cus, busy0, tsc0;
    uint64_t t = ticks();
    if(shm->decoded < last_dec || shm->converted < last_conv || convert_us < last_cus){   /* a reset: start over */
        last_dec = shm->decoded; last_conv = shm->converted; last_drop = shm->dropped;
        last_dus = shm->decode_us; last_cus = convert_us; last_t = t;
        return;
    }
    if(t < last_t + 500) return;
    uint64_t dec = shm->decoded - last_dec, conv = shm->converted - last_conv, drop = shm->dropped - last_drop;
    uint64_t dus = shm->decode_us - last_dus, cus = convert_us - last_cus;
    if(dec || conv) say("stats: %llu decoded, %llu shown, %llu late in %.1f s; decode %.1f ms, convert %.1f ms a picture (to %ux%u); heap %u MB",
        (unsigned long long)dec, (unsigned long long)conv, (unsigned long long)drop, (t - last_t) / 100.0,
        dec ? (double)(dus - cus) / 1000.0 / (double)dec : 0.0, conv ? (double)cus / 1000.0 / (double)conv : 0.0,
        (unsigned)want_w, (unsigned)want_h, (unsigned)(reel_heap_used() >> 20));
    last_t = t; last_dec = shm->decoded; last_conv = shm->converted; last_drop = shm->dropped; last_dus = shm->decode_us; last_cus = convert_us;
    (void)busy0; (void)tsc0;
}

/* ---- audio ---- */
static int16_t abuf[8192 * 2];

static uint64_t ring_room(void){
    uint64_t used = shm->pcm_w - shm->pcm_r;
    return used >= REEL_PCM_FRAMES ? 0 : REEL_PCM_FRAMES - used;
}

static void ring_put(const int16_t *pcm, uint64_t n){
    int16_t *ring = PCM_RING;
    uint64_t w = shm->pcm_w;
    while(n){
        uint64_t at_ = w % REEL_PCM_FRAMES, take = REEL_PCM_FRAMES - at_;
        if(take > n) take = n;
        if(pcm) memcpy(ring + at_ * 2, pcm, take * 4);
        else memset(ring + at_ * 2, 0, take * 4);
        if(pcm) pcm += take * 2;
        w += take; n -= take;
    }
    __asm__ volatile("" ::: "memory");
    shm->pcm_w = w;
}

/* puts decoded audio at its time on the ring's timeline */
static void place_audio(const int16_t *pcm, int n, int64_t apts){
    int64_t expect = shm->pcm_t0 + (int64_t)(shm->pcm_w - shm->pcm_p0) * 1000000 / 48000;
    int64_t diff = apts - expect;
    if(diff < -20000){                         /* overlaps what is already there: cut */
        int64_t skip = -diff * 48000 / 1000000;
        if(skip >= n) return;
        pcm += skip * 2; n -= (int)skip;
    } else if(diff > 20000){                   /* a hole: silence, so the timeline holds */
        uint64_t sil = (uint64_t)(diff * 48000 / 1000000);
        if(sil > 48000) sil = 48000;
        uint64_t room = ring_room();
        if(sil > room) sil = room;
        ring_put(NULL, sil);
    }
    uint64_t room = ring_room();
    if((uint64_t)n > room) n = (int)room;      /* cannot happen: callers check room */
    ring_put(pcm, (uint64_t)n);
}

static int audio_step(void){
    if(!ad || !at || a_done) return 0;
    int did = 0;
    for(int rounds = 0; rounds < 64; rounds++){
        if(ring_room() < 8192) break;
        int64_t apts;
        int n = mdec_audio(ad, abuf, 8192, &apts);
        if(n < 0){ say("audio: decoding failed"); a_done = 1; break; }
        if(n > 0){
            /* the part before the seek point is not played */
            int64_t end = apts + (int64_t)n * 1000000 / 48000;
            if(end <= pos_us){ did = 1; continue; }
            if(apts < pos_us){
                int skip = (int)((pos_us - apts) * 48000 / 1000000);
                if(skip < n){ place_audio(abuf + skip * 2, n - skip, pos_us); }
            } else place_audio(abuf, n, apts);
            did = 1;
            continue;
        }
        if(a_drained){ a_done = 1; break; }
        int idx;
        if(a_fresh){
            idx = sbuf_seek_index(at, pos_us);
            if(idx < 0) break;
            /* a little before, so the decoder is warmed up (AAC overlaps frames) */
            if(idx > 0) idx--;
            a_fresh = 0;
        } else idx = sbuf_next_index(at, a_last_dts);
        if(idx >= at->n){
            if(eos && !a_drained){ mdec_drain(ad); a_drained = 1; did = 1; continue; }
            break;
        }
        msample_t s = at->s[idx];
        int r = mdec_send(ad, &s);
        if(r == 1) continue;
        a_last_dts = s.dts;
        did = 1;
    }
    return did;
}

/* ---- commands ---- */
static uint32_t last_cmd_seq;

static void redraw(int64_t pts){
    int k = free_slot();
    if(k < 0) return;
    for(int i = 0; i < REEL_SLOTS; i++){
        if(keep[i].valid && keep[i].pic.pts == pts){
            if(i != k){ keep_t t = keep[k]; keep[k] = keep[i]; keep[i] = t; }
            fill_slot(k, &keep[k].pic);
            return;
        }
    }
}

static void handle_cmd(void){
    uint32_t seq = shm->cmd_seq;
    if(seq == last_cmd_seq) return;
    last_cmd_seq = seq;
    uint32_t cmd = shm->cmd;
    int a = shm->a, b = shm->b;
    switch(cmd){
    case R_CMD_RESET: reset_all(); break;
    case R_CMD_APPEND:
        if(a >= 0 && a < REEL_SBS && b > 0 && (uint32_t)b <= REEL_IN_BYTES){
            shm->sb[a].used = 1;
            eos = 0;                       /* more data: the stream is open again (MSE) */
            shm->ended = 0;
            int had = sb[a].error;
            if(sbuf_append(&sb[a], IN_AREA, (uint32_t)b) != 0 && !had){
                error("stream %d: %s", a, sb[a].errmsg);
                say("stream %d: %s", a, sb[a].errmsg);
            }
            pick_tracks();
            evict();
        }
        break;
    case R_CMD_REMOVE:
        if(a >= 0 && a < REEL_SBS) sbuf_remove(&sb[a], shm->t0, shm->t1);
        break;
    case R_CMD_ABORT:
        if(a >= 0 && a < REEL_SBS) sbuf_abort(&sb[a]);
        break;
    case R_CMD_SEEK: do_seek(shm->t0); break;
    case R_CMD_SIZE:
        want_w = a > 0 ? (uint32_t)a : 0; want_h = b > 0 ? (uint32_t)b : 0;
        smooth = shm->smooth != 0;
        shm->want_w = want_w; shm->want_h = want_h;
        break;
    case R_CMD_EOS: eos = 1; break;
    case R_CMD_REDRAW:
        want_w = a > 0 ? (uint32_t)a : want_w; want_h = b > 0 ? (uint32_t)b : want_h;
        smooth = shm->smooth != 0;
        redraw(shm->t0);
        break;
    case R_CMD_QUIT: sc3(SYS_EXIT, 0, 0, 0); break;
    }
    publish_status();
    __asm__ volatile("" ::: "memory");
    shm->ack_seq = seq;
}

static void calibrate(void){
    uint64_t t = ticks();
    while(ticks() == t) ;
    uint64_t c0 = rdtsc(), t0 = ticks();
    while(ticks() < t0 + 5) ;
    uint64_t c1 = rdtsc();
    tsc_per_us = (c1 - c0) / 50000;
    if(!tsc_per_us) tsc_per_us = 1;
}

__attribute__((noreturn)) void reel_main(void){
    if(shm->magic != REEL_MAGIC || shm->ver != REEL_VER) reel_fatal("wrong shared-memory version");
    reel_heap_init((void *)REEL_HEAP_VA, shm->heap_bytes);
    av_log_set_callback(av_log_cb);
    for(int i = 0; i < REEL_SBS; i++) sbuf_init(&sb[i]);
    calibrate();
    reset_all();
    publish_status();
    shm->state = R_ST_READY;
    uint64_t last_status = 0;
    for(;;){
        handle_cmd();
        int work = audio_step();
        work |= video_step();
        if(eos && (!vd || v_done) && (!ad || a_done)) shm->ended = 1;
        if(shm->playing) stats();
        uint64_t t = ticks();
        if(t != last_status){ publish_status(); last_status = t; }
        if(!work && shm->cmd_seq == last_cmd_seq) nap();
    }
}

/* the entry point: the kernel starts us here with the stack set up */
__asm__(".section .text.entry\n.global _start\n_start:\n  andq $-16, %rsp\n  call reel_main\n  ud2\n.previous");
