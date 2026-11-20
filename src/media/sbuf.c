/* sbuf.c — an MSE-style source buffer: fragmented MP4 and WebM in, coded frames by time out.
 *
 * Bytes arrive in arbitrary pieces. Only whole top-level structures are parsed: in MP4 the
 * init segment's moov, then each moof together with the mdat that follows it; in WebM the
 * EBML header, the Segment's Info and Tracks, and each Cluster child (or a whole Cluster
 * when its size is known). Every media segment is copied once into a block of its own and
 * its frames point into that block, so evicting old frames frees whole blocks.
 *
 * Written for this OS; nothing here comes from another project. */
#include "media.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const char *media_codec_name(int c){
    switch(c){
    case MC_H264: return "H.264"; case MC_VP9: return "VP9"; case MC_AV1: return "AV1";
    case MC_AAC: return "AAC"; case MC_OPUS: return "Opus";
    }
    return "?";
}

static uint32_t be16(const uint8_t *p){ return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p){ return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int fail(sbuf_t *b, const char *msg){
    b->error = 1;
    snprintf(b->errmsg, sizeof b->errmsg, "%s", msg);
    return -1;
}

void sbuf_init(sbuf_t *b){
    memset(b, 0, sizeof *b);
    b->wm_timescale = 1000000;
    b->duration_us = -1;
}

static void seg_unref(sbuf_t *b, mseg_t *s){
    if(s && --s->refs <= 0){
        b->bytes_held -= s->len;
        free(s->data);
        free(s);
    }
}

void sbuf_free(sbuf_t *b){
    for(int i = 0; i < b->ntracks; i++){
        mtrack_t *t = &b->tracks[i];
        for(int k = 0; k < t->n; k++) seg_unref(b, t->s[k].seg);
        free(t->s);
        free(t->extra);
    }
    free(b->in);
    sbuf_init(b);
}

void sbuf_abort(sbuf_t *b){
    b->in_len = 0;
    b->wm_in_cluster = 0;
}

mtrack_t *sbuf_track(sbuf_t *b, int kind){
    for(int i = 0; i < b->ntracks; i++) if(b->tracks[i].kind == kind && b->tracks[i].codec) return &b->tracks[i];
    return NULL;
}

static mtrack_t *track_by_id(sbuf_t *b, int id){
    for(int i = 0; i < b->ntracks; i++) if(b->tracks[i].id == id) return &b->tracks[i];
    return NULL;
}

/* ---- the frame list of a track: sorted by dts ---- */

/* first index with dts >= v */
static int lower_dts(const mtrack_t *t, int64_t v){
    int lo = 0, hi = t->n;
    while(lo < hi){ int m = (lo + hi) / 2; if(t->s[m].dts < v) lo = m + 1; else hi = m; }
    return lo;
}

int sbuf_next_index(const mtrack_t *t, int64_t dts){
    int lo = 0, hi = t->n;
    while(lo < hi){ int m = (lo + hi) / 2; if(t->s[m].dts <= dts) lo = m + 1; else hi = m; }
    return lo;
}

static void remove_range_idx(sbuf_t *b, mtrack_t *t, int from, int to){
    if(to <= from) return;
    for(int k = from; k < to; k++) seg_unref(b, t->s[k].seg);
    memmove(t->s + from, t->s + to, (size_t)(t->n - to) * sizeof *t->s);
    t->n -= to - from;
    t->gen++;
}

/* Adds a run of frames (decode order) from one media segment. Frames already buffered in
 * the run's time span are replaced, as MSE does when a range is appended again. */
static int add_frames(sbuf_t *b, mtrack_t *t, msample_t *fr, int nf){
    if(nf <= 0) return 0;
    int64_t lo = fr[0].dts, hi = fr[nf - 1].dts + (fr[nf - 1].dur > 0 ? fr[nf - 1].dur : 1);
    int a = lower_dts(t, lo), z = lower_dts(t, hi);
    if(z > a) remove_range_idx(b, t, a, z);
    if(t->n + nf > t->cap){
        int nc = t->cap ? t->cap * 2 : 1024;
        while(nc < t->n + nf) nc *= 2;
        msample_t *ns = realloc(t->s, (size_t)nc * sizeof *ns);
        if(!ns) return fail(b, "out of memory (frame list)");
        t->s = ns; t->cap = nc;
    }
    int at = lower_dts(t, lo);
    if(at < t->n){ memmove(t->s + at + nf, t->s + at, (size_t)(t->n - at) * sizeof *t->s); t->gen++; }
    memcpy(t->s + at, fr, (size_t)nf * sizeof *fr);
    t->n += nf;
    /* the frame before the run gets its duration from the run when it had none */
    if(at > 0 && t->s[at - 1].dur <= 0) t->s[at - 1].dur = fr[0].dts - t->s[at - 1].dts;
    return 0;
}

static mseg_t *seg_new(sbuf_t *b, const uint8_t *data, uint32_t len){
    mseg_t *s = malloc(sizeof *s);
    if(!s) return NULL;
    s->data = malloc(len ? len : 1);
    if(!s->data){ free(s); return NULL; }
    memcpy(s->data, data, len);
    s->len = len;
    s->refs = 0;
    b->bytes_held += len;
    return s;
}

static int cmp_range(const void *a, const void *b){
    int64_t x = ((const int64_t *)a)[0], y = ((const int64_t *)b)[0];
    return x < y ? -1 : x > y;
}

int sbuf_buffered(const mtrack_t *t, int64_t *r, int max){
    if(!t || t->n == 0 || max <= 0) return 0;
    /* by presentation time: frames sorted by pts, joined where they touch */
    int64_t *iv = malloc((size_t)t->n * 2 * sizeof *iv);
    if(!iv) return 0;
    for(int k = 0; k < t->n; k++){ iv[k * 2] = t->s[k].pts; iv[k * 2 + 1] = t->s[k].pts + (t->s[k].dur > 0 ? t->s[k].dur : 0); }
    qsort(iv, (size_t)t->n, 2 * sizeof *iv, cmp_range);
    int nr = 0;
    int64_t s = iv[0], e = iv[1];
    const int64_t slack = 100000;               /* gaps under 0.1 s are not gaps */
    for(int k = 1; k < t->n; k++){
        int64_t fs = iv[k * 2], fe = iv[k * 2 + 1];
        if(fs <= e + slack){ if(fe > e) e = fe; continue; }
        if(nr < max){ r[nr * 2] = s; r[nr * 2 + 1] = e; nr++; }
        s = fs; e = fe;
    }
    if(nr < max){ r[nr * 2] = s; r[nr * 2 + 1] = e; nr++; }
    free(iv);
    return nr;
}

void sbuf_remove(sbuf_t *b, int64_t start, int64_t end){
    for(int i = 0; i < b->ntracks; i++){
        mtrack_t *t = &b->tracks[i];
        /* whole GOPs only, so what stays still starts with a key frame */
        int k = 0;
        while(k < t->n){
            if(t->s[k].pts >= start && t->s[k].pts < end){
                int j = k + 1;
                if(t->kind == MK_VIDEO){ while(j < t->n && !t->s[j].key) j++; }
                else while(j < t->n && t->s[j].pts >= start && t->s[j].pts < end) j++;
                remove_range_idx(b, t, k, j);
            } else k++;
        }
    }
}

int sbuf_seek_index(const mtrack_t *t, int64_t at){
    if(!t || t->n == 0) return -1;
    int best = -1;
    if(t->kind == MK_VIDEO){
        for(int k = 0; k < t->n; k++){
            if(t->s[k].key && t->s[k].pts <= at + 1000) best = k;
            if(t->s[k].dts > at + 1000) break;
        }
        if(best < 0 && t->s[0].key && t->s[0].pts <= at + 500000) best = 0;   /* just after: fine */
        return best;
    }
    for(int k = 0; k < t->n; k++){
        if(t->s[k].pts <= at) best = k;
        else break;
    }
    if(best >= 0 && t->s[best].pts + t->s[best].dur + 100000 < at) return -1;
    if(best < 0 && t->s[0].pts <= at + 500000) best = 0;
    return best;
}

/* ---- input bytes ---- */
static int in_add(sbuf_t *b, const uint8_t *d, uint32_t n){
    if(b->in_len + n > b->in_cap){
        uint32_t nc = b->in_cap ? b->in_cap : 65536;
        while(nc < b->in_len + n) nc *= 2;
        uint8_t *ni = realloc(b->in, nc);
        if(!ni) return fail(b, "out of memory (input)");
        b->in = ni; b->in_cap = nc;
    }
    memcpy(b->in + b->in_len, d, n);
    b->in_len += n;
    return 0;
}
static void in_eat(sbuf_t *b, uint32_t n){
    memmove(b->in, b->in + n, b->in_len - n);
    b->in_len -= n;
}

/* ===================== fragmented MP4 ===================== */

/* walks the boxes in [p, end): calls fn for each; returns -1 when they do not fit */
typedef struct { const uint8_t *p; uint64_t size; uint32_t hdr; char type[5]; } box_t;
static int box_at(const uint8_t *p, const uint8_t *end, box_t *bx){
    if(end - p < 8) return -1;
    uint64_t sz = be32(p);
    uint32_t hdr = 8;
    if(sz == 1){ if(end - p < 16) return -1; sz = be64(p + 8); hdr = 16; }
    else if(sz == 0) sz = (uint64_t)(end - p);
    if(sz < hdr || sz > (uint64_t)(end - p)) return -1;
    bx->p = p; bx->size = sz; bx->hdr = hdr;
    memcpy(bx->type, p + 4, 4); bx->type[4] = 0;
    return 0;
}
static int is(const box_t *bx, const char *t){ return memcmp(bx->type, t, 4) == 0; }
#define BOX_BODY(bx) ((bx)->p + (bx)->hdr)
#define BOX_END(bx)  ((bx)->p + (bx)->size)

/* the first child box of a type inside [p, end) */
static int find_box(const uint8_t *p, const uint8_t *end, const char *type, box_t *out){
    box_t bx;
    while(p < end && box_at(p, end, &bx) == 0){
        if(is(&bx, type)){ *out = bx; return 0; }
        p += bx.size;
    }
    return -1;
}

static void set_extra(mtrack_t *t, const uint8_t *p, uint32_t n){
    free(t->extra);
    t->extra = malloc(n + 64);           /* libavcodec wants padding after extradata */
    if(!t->extra){ t->extralen = 0; return; }
    memcpy(t->extra, p, n);
    memset(t->extra + n, 0, 64);
    t->extralen = (int)n;
}

/* esds: ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo (AudioSpecificConfig) */
static uint32_t desc_len(const uint8_t **pp, const uint8_t *end){
    uint32_t n = 0;
    for(int i = 0; i < 4 && *pp < end; i++){
        uint8_t c = *(*pp)++;
        n = n << 7 | (c & 0x7f);
        if(!(c & 0x80)) break;
    }
    return n;
}
static void parse_esds(mtrack_t *t, const uint8_t *p, const uint8_t *end){
    p += 4;                                  /* version, flags */
    while(p + 2 <= end){
        uint8_t tag = *p++;
        uint32_t len = desc_len(&p, end);
        const uint8_t *body = p;
        if(len > (uint32_t)(end - p)) len = (uint32_t)(end - p);
        if(tag == 0x03){                     /* ES_Descriptor */
            uint8_t fl = body[2];
            p = body + 3;
            if(fl & 0x80) p += 2;
            if(fl & 0x40) p += 1 + *p;
            if(fl & 0x20) p += 2;
            continue;                        /* its children follow */
        }
        if(tag == 0x04){ p = body + 13; continue; }     /* DecoderConfigDescriptor */
        if(tag == 0x05){ set_extra(t, body, len); return; }
        p = body + len;
    }
}

/* dOps (big-endian, in MP4) -> OpusHead (little-endian, what decoders take) */
static void parse_dops(mtrack_t *t, const uint8_t *p, const uint8_t *end){
    if(end - p < 11) return;
    uint8_t head[19 + 2 + 255];
    int ch = p[1];
    uint32_t preskip = be16(p + 2), rate = be32(p + 4);
    int16_t gain = (int16_t)be16(p + 8);
    int family = p[10];
    memcpy(head, "OpusHead", 8);
    head[8] = 1; head[9] = (uint8_t)ch;
    head[10] = preskip & 0xff; head[11] = preskip >> 8;
    head[12] = rate & 0xff; head[13] = rate >> 8 & 0xff; head[14] = rate >> 16 & 0xff; head[15] = rate >> 24;
    head[16] = (uint16_t)gain & 0xff; head[17] = (uint16_t)gain >> 8;
    head[18] = (uint8_t)family;
    uint32_t n = 19;
    if(family != 0 && end - p >= 13 + ch){
        memcpy(head + 19, p + 11, (size_t)(2 + ch));
        n += 2 + (uint32_t)ch;
    }
    set_extra(t, head, n);
    t->channels = ch;
}

static void parse_stsd(mtrack_t *t, const uint8_t *p, const uint8_t *end){
    p += 8;                                   /* version/flags, entry count */
    box_t e;
    if(box_at(p, end, &e) != 0) return;
    const uint8_t *body = BOX_BODY(&e), *eend = BOX_END(&e);
    box_t c;
    if(is(&e, "avc1") || is(&e, "avc3") || is(&e, "vp09") || is(&e, "av01")){
        if(eend - body < 78) return;
        t->width = (int)be16(body + 24);
        t->height = (int)be16(body + 26);
        const uint8_t *kids = body + 78;
        if(is(&e, "vp09")) t->codec = MC_VP9;
        else if(is(&e, "av01")){
            t->codec = MC_AV1;
            if(find_box(kids, eend, "av1C", &c) == 0) set_extra(t, BOX_BODY(&c), (uint32_t)(c.size - c.hdr));
        } else {
            if(find_box(kids, eend, "avcC", &c) == 0){ set_extra(t, BOX_BODY(&c), (uint32_t)(c.size - c.hdr)); t->codec = MC_H264; }
        }
    } else if(is(&e, "mp4a") || is(&e, "Opus")){
        if(eend - body < 28) return;
        t->channels = (int)be16(body + 16);
        t->rate = (int)(be32(body + 24) >> 16);
        const uint8_t *kids = body + 28;
        if(is(&e, "mp4a")){
            if(find_box(kids, eend, "esds", &c) == 0){ parse_esds(t, BOX_BODY(&c), BOX_END(&c)); if(t->extralen) t->codec = MC_AAC; }
        } else if(find_box(kids, eend, "dOps", &c) == 0){ parse_dops(t, BOX_BODY(&c), BOX_END(&c)); t->codec = MC_OPUS; t->rate = 48000; }
    }
}

static int parse_moov(sbuf_t *b, const uint8_t *p, const uint8_t *end){
    box_t bx;
    /* trex defaults first (they sit in mvex, after the traks) */
    struct { uint32_t id, dur, size, flags; } trex[SB_MAX_TRACKS];
    int ntrex = 0;
    box_t mvex;
    if(find_box(p, end, "mvex", &mvex) == 0){
        const uint8_t *q = BOX_BODY(&mvex);
        while(q < BOX_END(&mvex) && box_at(q, BOX_END(&mvex), &bx) == 0){
            if(is(&bx, "trex") && bx.size >= 32 && ntrex < SB_MAX_TRACKS){
                const uint8_t *d = BOX_BODY(&bx);
                trex[ntrex].id = be32(d + 4); trex[ntrex].dur = be32(d + 12);
                trex[ntrex].size = be32(d + 16); trex[ntrex].flags = be32(d + 20);
                ntrex++;
            }
            q += bx.size;
        }
    }
    int was_init = b->have_init;
    while(p < end && box_at(p, end, &bx) == 0){
        if(is(&bx, "trak")){
            box_t tkhd, mdia, mdhd, hdlr, minf, stbl, stsd, edts, elst;
            const uint8_t *tb = BOX_BODY(&bx), *te = BOX_END(&bx);
            if(find_box(tb, te, "tkhd", &tkhd) || find_box(tb, te, "mdia", &mdia)){ p += bx.size; continue; }
            const uint8_t *th = BOX_BODY(&tkhd);
            int id = (int)be32(th + (th[0] == 1 ? 20 : 12));
            mtrack_t *t = track_by_id(b, id);
            if(!t){
                if(b->ntracks == SB_MAX_TRACKS){ p += bx.size; continue; }
                t = &b->tracks[b->ntracks++];
                memset(t, 0, sizeof *t);
                t->id = id;
            }
            const uint8_t *mb = BOX_BODY(&mdia), *me = BOX_END(&mdia);
            if(find_box(mb, me, "mdhd", &mdhd) == 0){
                const uint8_t *d = BOX_BODY(&mdhd);
                t->timescale = be32(d + (d[0] == 1 ? 20 : 12));
            }
            if(!t->timescale) t->timescale = 1000;
            if(find_box(mb, me, "hdlr", &hdlr) == 0){
                const uint8_t *d = BOX_BODY(&hdlr);
                if(!memcmp(d + 8, "vide", 4)) t->kind = MK_VIDEO;
                else if(!memcmp(d + 8, "soun", 4)) t->kind = MK_AUDIO;
            }
            if(find_box(mb, me, "minf", &minf) == 0 && find_box(BOX_BODY(&minf), BOX_END(&minf), "stbl", &stbl) == 0
               && find_box(BOX_BODY(&stbl), BOX_END(&stbl), "stsd", &stsd) == 0)
                parse_stsd(t, BOX_BODY(&stsd), BOX_END(&stsd));
            /* an edit list that starts the track later (B-frame delay, AAC priming) */
            t->shift = 0;
            if(find_box(tb, te, "edts", &edts) == 0 && find_box(BOX_BODY(&edts), BOX_END(&edts), "elst", &elst) == 0){
                const uint8_t *d = BOX_BODY(&elst);
                uint32_t cnt = be32(d + 4);
                const uint8_t *e = d + 8;
                for(uint32_t i = 0; i < cnt && e + (d[0] == 1 ? 20 : 12) <= BOX_END(&elst); i++){
                    int64_t mt = d[0] == 1 ? (int64_t)be64(e + 8) : (int64_t)(int32_t)be32(e + 4);
                    e += d[0] == 1 ? 20 : 12;
                    if(mt >= 0){ t->shift = mt; break; }        /* skip empty edits */
                }
            }
            for(int i = 0; i < ntrex; i++) if((int)trex[i].id == id){ t->def_dur = trex[i].dur; t->def_size = trex[i].size; t->def_flags = trex[i].flags; }
        } else if(is(&bx, "mvhd")){
            const uint8_t *d = BOX_BODY(&bx);
            uint32_t ts = be32(d + (d[0] == 1 ? 20 : 12));
            uint64_t du = d[0] == 1 ? be64(d + 24) : be32(d + 16);
            if(ts && du && du != 0xffffffffu && du != ~0ull) b->duration_us = (int64_t)(du * 1000000 / ts);
        }
        p += bx.size;
    }
    if(!b->ntracks) return fail(b, "the MP4 has no tracks");
    b->have_init = 1;
    (void)was_init;
    return 0;
}

static int64_t ticks_us(const mtrack_t *t, int64_t v){
    v -= t->shift;
    /* v * 1e6 / timescale without overflow for any sane value */
    int64_t q = v / t->timescale, r = v % t->timescale;
    return q * 1000000 + r * 1000000 / t->timescale;
}

/* one moof and its mdat, both whole, starting at seg->data */
static int parse_fragment(sbuf_t *b, mseg_t *seg, uint32_t moof_len){
    const uint8_t *base = seg->data, *end = seg->data + seg->len;
    const uint8_t *p = base + 8, *mend = base + moof_len;
    box_t traf;
    static msample_t *fr = NULL;             /* scratch for one run */
    static int frcap = 0;
    while(p < mend && box_at(p, mend, &traf) == 0){
        p += traf.size;
        if(!is(&traf, "traf")) continue;
        const uint8_t *tp = BOX_BODY(&traf), *tend = BOX_END(&traf);
        box_t bx;
        mtrack_t *t = NULL;
        uint32_t dflags = 0, ddur = 0, dsize = 0, tfhd_flags = 0;
        uint64_t data_base = 0;
        int64_t tdts = 0;
        int have_tfdt = 0;
        if(find_box(tp, tend, "tfhd", &bx) != 0) continue;
        {
            const uint8_t *d = BOX_BODY(&bx);
            tfhd_flags = be32(d) & 0xffffff;
            t = track_by_id(b, (int)be32(d + 4));
            if(!t) continue;
            dflags = t->def_flags; ddur = t->def_dur; dsize = t->def_size;
            const uint8_t *q = d + 8;
            if(tfhd_flags & 0x1){ data_base = be64(q); q += 8; }
            if(tfhd_flags & 0x2) q += 4;
            if(tfhd_flags & 0x8){ ddur = be32(q); q += 4; }
            if(tfhd_flags & 0x10){ dsize = be32(q); q += 4; }
            if(tfhd_flags & 0x20){ dflags = be32(q); q += 4; }
        }
        (void)data_base;              /* absolute file offsets mean nothing in a stream: offsets
                                         are taken from the moof, as default-base-is-moof says */
        if(find_box(tp, tend, "tfdt", &bx) == 0){
            const uint8_t *d = BOX_BODY(&bx);
            tdts = d[0] == 1 ? (int64_t)be64(d + 4) : (int64_t)be32(d + 4);
            have_tfdt = 1;
        }
        if(!have_tfdt){
            /* continue from the track's last frame */
            if(t->n) tdts = -1;
        }
        const uint8_t *q = tp;
        int64_t run_dts = tdts;
        while(q < tend && box_at(q, tend, &bx) == 0){
            q += bx.size;
            if(!is(&bx, "trun")) continue;
            const uint8_t *d = BOX_BODY(&bx);
            uint32_t fl = be32(d) & 0xffffff, cnt = be32(d + 4);
            int ver = d[0];
            const uint8_t *r = d + 8;
            int32_t doff = 0;
            uint32_t first_flags = dflags;
            int has_first = 0;
            if(fl & 0x1){ doff = (int32_t)be32(r); r += 4; }
            if(fl & 0x4){ first_flags = be32(r); r += 4; has_first = 1; }
            uint32_t per = ((fl & 0x100) ? 4 : 0) + ((fl & 0x200) ? 4 : 0) + ((fl & 0x400) ? 4 : 0) + ((fl & 0x800) ? 4 : 0);
            if((uint64_t)cnt * per > (uint64_t)(BOX_END(&bx) - r)) return fail(b, "a trun box is cut short");
            if((int)cnt > frcap){
                int nc = frcap ? frcap : 256;
                while(nc < (int)cnt) nc *= 2;
                msample_t *nf = realloc(fr, (size_t)nc * sizeof *nf);
                if(!nf) return fail(b, "out of memory");
                fr = nf; frcap = nc;
            }
            const uint8_t *data = base + doff;
            int nf = 0;
            for(uint32_t i = 0; i < cnt; i++){
                uint32_t dur = ddur, size = dsize, sfl = (i == 0 && has_first) ? first_flags : dflags;
                int32_t cto = 0;
                if(fl & 0x100){ dur = be32(r); r += 4; }
                if(fl & 0x200){ size = be32(r); r += 4; }
                if(fl & 0x400){ sfl = be32(r); r += 4; }
                if(fl & 0x800){ cto = ver ? (int32_t)be32(r) : (int32_t)be32(r); r += 4; }
                if(data < base || data + size > end) return fail(b, "a frame lies outside its mdat");
                if(run_dts < 0){       /* no tfdt: after the last frame we have */
                    const msample_t *l = &t->s[t->n - 1];
                    run_dts = l->dts * (int64_t)t->timescale / 1000000 + t->shift + (int64_t)l->dur * t->timescale / 1000000;
                }
                msample_t *s = &fr[nf++];
                s->dts = ticks_us(t, run_dts);
                s->pts = ticks_us(t, run_dts + cto);
                s->dur = ticks_us(t, run_dts + dur) - s->dts;
                s->data = data;
                s->len = size;
                s->key = t->kind == MK_AUDIO ? 1 : !(sfl & 0x10000);
                s->seg = seg;
                seg->refs++;
                data += size;
                run_dts += dur;
            }
            if(add_frames(b, t, fr, nf) != 0) return -1;
        }
    }
    return 0;
}

static int mp4_step(sbuf_t *b){
    box_t bx;
    const uint8_t *p = b->in, *end = b->in + b->in_len;
    if(b->in_len < 8) return 0;
    uint64_t sz = be32(p);
    if(sz == 1){ if(b->in_len < 16) return 0; sz = be64(p + 8); }
    if(sz < 8 || sz > 512u * 1024 * 1024) return fail(b, "a box in the MP4 has an impossible size");
    if(sz > b->in_len) return 0;                        /* wait for the rest */
    box_at(p, end, &bx);
    if(is(&bx, "moov")){
        if(parse_moov(b, BOX_BODY(&bx), BOX_END(&bx)) != 0) return -1;
        in_eat(b, (uint32_t)sz);
        return 1;
    }
    if(is(&bx, "moof")){
        if(!b->have_init) return fail(b, "a media segment came before the init segment");
        /* the mdat after it (boxes between them are allowed) */
        const uint8_t *q = p + sz;
        for(;;){
            if(end - q < 8) return 0;
            uint64_t s2 = be32(q);
            if(s2 == 1){ if(end - q < 16) return 0; s2 = be64(q + 8); }
            if(s2 < 8) return fail(b, "a box in the MP4 has an impossible size");
            if(s2 > (uint64_t)(end - q)) return 0;
            if(!memcmp(q + 4, "mdat", 4)){ q += s2; break; }
            q += s2;
        }
        uint32_t whole = (uint32_t)(q - p);
        mseg_t *seg = seg_new(b, p, whole);
        if(!seg) return fail(b, "out of memory (segment)");
        int r = parse_fragment(b, seg, (uint32_t)sz);
        if(seg->refs == 0){ b->bytes_held -= seg->len; free(seg->data); free(seg); }
        if(r != 0) return -1;
        in_eat(b, whole);
        return 1;
    }
    /* ftyp, sidx, styp, emsg, free, a stray mdat: nothing for us */
    in_eat(b, (uint32_t)sz);
    return 1;
}

/* ===================== WebM / Matroska ===================== */

#define ID_EBML        0x1A45DFA3
#define ID_SEGMENT     0x18538067
#define ID_INFO        0x1549A966
#define ID_TIMESCALE   0x2AD7B1
#define ID_DURATION    0x4489
#define ID_TRACKS      0x1654AE6B
#define ID_TRACKENTRY  0xAE
#define ID_TRACKNUM    0xD7
#define ID_TRACKTYPE   0x83
#define ID_CODECID     0x86
#define ID_CODECPRIV   0x63A2
#define ID_CODECDELAY  0x56AA
#define ID_VIDEO       0xE0
#define ID_PIXW        0xB0
#define ID_PIXH        0xBA
#define ID_AUDIO       0xE1
#define ID_SAMPLERATE  0xB5
#define ID_CHANNELS    0x9F
#define ID_DEFDUR      0x23E383
#define ID_CLUSTER     0x1F43B675
#define ID_TIMECODE    0xE7
#define ID_SIMPLEBLOCK 0xA3
#define ID_BLOCKGROUP  0xA0
#define ID_BLOCK       0xA1
#define ID_BLOCKDUR    0x9B
#define ID_REFBLOCK    0xFB

#define UNKNOWN_SIZE (~0ull)

/* an EBML id (the marker bits kept); 0 when cut short */
static int ebml_id(const uint8_t *p, const uint8_t *end, uint32_t *id){
    if(p >= end) return 0;
    int n = 1;
    uint8_t c = p[0];
    while(n <= 4 && !(c & (0x80 >> (n - 1)))) n++;
    if(n > 4) return -1;
    if(end - p < n) return 0;
    uint32_t v = 0;
    for(int i = 0; i < n; i++) v = v << 8 | p[i];
    *id = v;
    return n;
}
/* a size (marker removed); all ones means unknown */
static int ebml_size(const uint8_t *p, const uint8_t *end, uint64_t *sz){
    if(p >= end) return 0;
    int n = 1;
    uint8_t c = p[0];
    while(n <= 8 && !(c & (0x80 >> (n - 1)))) n++;
    if(n > 8) return -1;
    if(end - p < n) return 0;
    uint64_t v = c & (0xff >> n);
    int ones = v == (uint64_t)(0xff >> n);
    for(int i = 1; i < n; i++){ v = v << 8 | p[i]; if(p[i] != 0xff) ones = 0; }
    *sz = ones ? UNKNOWN_SIZE : v;
    return n;
}
static uint64_t ebml_uint(const uint8_t *p, uint64_t n){ uint64_t v = 0; for(uint64_t i = 0; i < n && i < 8; i++) v = v << 8 | p[i]; return v; }
static double ebml_float(const uint8_t *p, uint64_t n){
    if(n == 4){ union { uint32_t u; float f; } x; x.u = (uint32_t)ebml_uint(p, 4); return x.f; }
    if(n == 8){ union { uint64_t u; double d; } x; x.u = ebml_uint(p, 8); return x.d; }
    return 0;
}

typedef struct { uint32_t id; uint64_t size; uint32_t hdr; const uint8_t *body; } el_t;
/* 1 got it, 0 need more bytes, -1 broken */
static int el_head(const uint8_t *p, const uint8_t *end, el_t *e){
    int a = ebml_id(p, end, &e->id);
    if(a <= 0) return a;
    int s = ebml_size(p + a, end, &e->size);
    if(s <= 0) return s;
    e->hdr = (uint32_t)(a + s);
    e->body = p + a + s;
    return 1;
}

static void parse_track_entry(sbuf_t *b, const uint8_t *p, const uint8_t *end){
    mtrack_t tmp;
    memset(&tmp, 0, sizeof tmp);
    char codec[32] = "";
    double rate = 0;
    el_t e;
    while(p < end && el_head(p, end, &e) == 1 && e.size != UNKNOWN_SIZE && e.size <= (uint64_t)(end - e.body)){
        const uint8_t *d = e.body;
        switch(e.id){
        case ID_TRACKNUM: tmp.id = (int)ebml_uint(d, e.size); break;
        case ID_TRACKTYPE: { uint64_t ty = ebml_uint(d, e.size); tmp.kind = ty == 1 ? MK_VIDEO : ty == 2 ? MK_AUDIO : MK_NONE; } break;
        case ID_CODECID: { uint64_t n = e.size < sizeof codec - 1 ? e.size : sizeof codec - 1; memcpy(codec, d, n); codec[n] = 0; } break;
        case ID_CODECPRIV: set_extra(&tmp, d, (uint32_t)e.size); break;
        case ID_CODECDELAY: tmp.codec_delay = (int64_t)(ebml_uint(d, e.size) / 1000); break;
        case ID_DEFDUR: tmp.def_dur = (uint32_t)(ebml_uint(d, e.size) / 1000); break;
        case ID_VIDEO: {
            const uint8_t *q = d, *qe = d + e.size;
            el_t v;
            while(q < qe && el_head(q, qe, &v) == 1 && v.size <= (uint64_t)(qe - v.body)){
                if(v.id == ID_PIXW) tmp.width = (int)ebml_uint(v.body, v.size);
                if(v.id == ID_PIXH) tmp.height = (int)ebml_uint(v.body, v.size);
                q = v.body + v.size;
            }
        } break;
        case ID_AUDIO: {
            const uint8_t *q = d, *qe = d + e.size;
            el_t v;
            while(q < qe && el_head(q, qe, &v) == 1 && v.size <= (uint64_t)(qe - v.body)){
                if(v.id == ID_SAMPLERATE) rate = ebml_float(v.body, v.size);
                if(v.id == ID_CHANNELS) tmp.channels = (int)ebml_uint(v.body, v.size);
                q = v.body + v.size;
            }
        } break;
        }
        p = e.body + e.size;
    }
    tmp.rate = (int)rate;
    if(!strcmp(codec, "V_VP9")) tmp.codec = MC_VP9;
    else if(!strcmp(codec, "V_AV1")) tmp.codec = MC_AV1;
    else if(!strcmp(codec, "V_MPEG4/ISO/AVC")) tmp.codec = MC_H264;
    else if(!strcmp(codec, "A_OPUS")){ tmp.codec = MC_OPUS; tmp.rate = 48000; }
    else if(!strcmp(codec, "A_AAC")) tmp.codec = MC_AAC;
    tmp.timescale = 1000000;
    mtrack_t *t = track_by_id(b, tmp.id);
    if(!t){
        if(b->ntracks == SB_MAX_TRACKS){ free(tmp.extra); return; }
        t = &b->tracks[b->ntracks++];
        *t = tmp;
        return;
    }
    /* a new init segment for a track we have: keep its frames, take the new config */
    free(t->extra);
    t->extra = tmp.extra; t->extralen = tmp.extralen;
    t->kind = tmp.kind; t->codec = tmp.codec;
    t->width = tmp.width; t->height = tmp.height; t->rate = tmp.rate; t->channels = tmp.channels;
    t->codec_delay = tmp.codec_delay; t->def_dur = tmp.def_dur;
}

/* lacing: splits a block's payload into frames; returns how many (0 on error) */
static int unlace(const uint8_t *p, const uint8_t *end, int lacing, const uint8_t **fp, uint32_t *fl, int max){
    if(lacing == 0){ fp[0] = p; fl[0] = (uint32_t)(end - p); return 1; }
    if(p >= end) return 0;
    int n = p[0] + 1;
    p++;
    if(n > max) return 0;
    uint64_t sizes[256];
    uint64_t total = 0;
    if(lacing == 1){                         /* Xiph */
        for(int i = 0; i < n - 1; i++){
            uint64_t s = 0;
            for(;;){ if(p >= end) return 0; uint8_t c = *p++; s += c; if(c != 255) break; }
            sizes[i] = s; total += s;
        }
    } else if(lacing == 3){                  /* EBML */
        uint64_t s;
        int k = ebml_size(p, end, &s);
        if(k <= 0) return 0;
        p += k; sizes[0] = s; total = s;
        for(int i = 1; i < n - 1; i++){
            uint64_t raw;
            k = ebml_size(p, end, &raw);
            if(k <= 0) return 0;
            p += k;
            int64_t bias = ((int64_t)1 << (7 * k - 1)) - 1;
            int64_t v = (int64_t)sizes[i - 1] + ((int64_t)raw - bias);
            if(v < 0) return 0;
            sizes[i] = (uint64_t)v; total += sizes[i];
        }
    } else {                                 /* fixed */
        uint64_t all = (uint64_t)(end - p);
        if(all % (uint64_t)n) return 0;
        for(int i = 0; i < n - 1; i++){ sizes[i] = all / (uint64_t)n; total += sizes[i]; }
    }
    if(total > (uint64_t)(end - p)) return 0;
    sizes[n - 1] = (uint64_t)(end - p) - total;
    for(int i = 0; i < n; i++){ fp[i] = p; fl[i] = (uint32_t)sizes[i]; p += sizes[i]; }
    return n;
}

/* A (Simple)Block inside seg; key: -1 = from the SimpleBlock flags */
static int add_block(sbuf_t *b, mseg_t *seg, const uint8_t *p, const uint8_t *end, int key, int64_t dur_ticks){
    uint64_t tn;
    int k = ebml_size(p, end, &tn);
    if(k <= 0 || end - p < k + 3) return fail(b, "a WebM block is cut short");
    mtrack_t *t = track_by_id(b, (int)tn);
    if(!t || !t->codec) return 0;            /* a track we do not play */
    p += k;
    int16_t rel = (int16_t)be16(p);
    uint8_t flags = p[2];
    p += 3;
    if(key < 0) key = (flags & 0x80) != 0;
    if(t->kind == MK_AUDIO) key = 1;
    const uint8_t *fp[256];
    uint32_t fl[256];
    int n = unlace(p, end, (flags >> 1) & 3, fp, fl, 256);
    if(n <= 0) return fail(b, "bad lacing in a WebM block");
    int64_t ts_ns = (int64_t)(b->wm_cluster_tc + rel) * (int64_t)b->wm_timescale;
    int64_t t0 = ts_ns / 1000 - t->codec_delay;
    int64_t each = t->def_dur ? (int64_t)t->def_dur
                 : dur_ticks > 0 ? dur_ticks * (int64_t)b->wm_timescale / 1000 / n : 0;
    msample_t fr[256];
    for(int i = 0; i < n; i++){
        msample_t *s = &fr[i];
        s->pts = s->dts = t0 + each * i;
        s->dur = each;
        s->data = fp[i]; s->len = fl[i];
        s->key = (uint8_t)(i == 0 ? key : t->kind == MK_AUDIO);
        s->seg = seg;
        seg->refs++;
    }
    /* no duration in the file: the next frame's start tells it */
    if(!each && t->n && t->s[t->n - 1].dur <= 0 && t->s[t->n - 1].dts < fr[0].dts)
        t->s[t->n - 1].dur = fr[0].dts - t->s[t->n - 1].dts;
    /* WebM is in presentation order; VP9/AV1/Opus have no reordering, so dts = pts */
    return add_frames(b, t, fr, n);
}

/* a BlockGroup's body */
static int add_group(sbuf_t *b, mseg_t *seg, const uint8_t *p, const uint8_t *end){
    el_t e;
    const uint8_t *blk = NULL, *blk_end = NULL;
    int has_ref = 0;
    int64_t dur = 0;
    while(p < end && el_head(p, end, &e) == 1 && e.size <= (uint64_t)(end - e.body)){
        if(e.id == ID_BLOCK){ blk = e.body; blk_end = e.body + e.size; }
        else if(e.id == ID_REFBLOCK) has_ref = 1;
        else if(e.id == ID_BLOCKDUR) dur = (int64_t)ebml_uint(e.body, e.size);
        p = e.body + e.size;
    }
    if(!blk) return 0;
    return add_block(b, seg, blk, blk_end, !has_ref, dur);
}

/* the children of a cluster in [p, end), all inside seg */
static int parse_cluster_kids(sbuf_t *b, mseg_t *seg, const uint8_t *p, const uint8_t *end){
    el_t e;
    while(p < end){
        int r = el_head(p, end, &e);
        if(r != 1 || e.size == UNKNOWN_SIZE || e.size > (uint64_t)(end - e.body)) return fail(b, "a WebM cluster is malformed");
        if(e.id == ID_TIMECODE) b->wm_cluster_tc = (int64_t)ebml_uint(e.body, e.size);
        else if(e.id == ID_SIMPLEBLOCK){ if(add_block(b, seg, e.body, e.body + e.size, -1, 0) != 0) return -1; }
        else if(e.id == ID_BLOCKGROUP){ if(add_group(b, seg, e.body, e.body + e.size) != 0) return -1; }
        p = e.body + e.size;
    }
    return 0;
}

static int webm_step(sbuf_t *b){
    const uint8_t *p = b->in, *end = b->in + b->in_len;
    el_t e;
    int r = el_head(p, end, &e);
    if(r == 0) return 0;
    if(r < 0) return fail(b, "the WebM stream is malformed");
    /* masters we step into rather than wait for */
    if(e.id == ID_SEGMENT){ b->wm_in_segment = 1; b->wm_in_cluster = 0; in_eat(b, e.hdr); return 1; }
    if(e.id == ID_CLUSTER && e.size == UNKNOWN_SIZE){ b->wm_in_cluster = 1; in_eat(b, e.hdr); return 1; }
    if(e.size == UNKNOWN_SIZE) return fail(b, "an unsized WebM element we cannot step into");
    if(e.size > 512u * 1024 * 1024) return fail(b, "a WebM element has an impossible size");
    uint64_t whole = e.hdr + e.size;
    /* things we skip need not be held whole: drop them as they come */
    int wanted = e.id == ID_EBML || e.id == ID_INFO || e.id == ID_TRACKS || e.id == ID_CLUSTER
              || e.id == ID_SIMPLEBLOCK || e.id == ID_BLOCKGROUP || e.id == ID_TIMECODE;
    if(!wanted){
        if(whole > b->in_len){
            /* skip what we have, remember how much more to skip by keeping a header... simpler:
               wait unless it is big (Cues can be ~100 KB; fine to wait) */
            return 0;
        }
        in_eat(b, (uint32_t)whole);
        return 1;
    }
    if(whole > b->in_len) return 0;
    switch(e.id){
    case ID_EBML: {
        /* DocType check is not needed: an EBML header starts a new stream */
        b->wm_in_segment = 0; b->wm_in_cluster = 0;
        break;
    }
    case ID_INFO: {
        const uint8_t *q = e.body, *qe = e.body + e.size;
        el_t c;
        double dur = -1;
        while(q < qe && el_head(q, qe, &c) == 1 && c.size <= (uint64_t)(qe - c.body)){
            if(c.id == ID_TIMESCALE) b->wm_timescale = ebml_uint(c.body, c.size);
            if(c.id == ID_DURATION) dur = ebml_float(c.body, c.size);
            q = c.body + c.size;
        }
        if(!b->wm_timescale) b->wm_timescale = 1000000;
        if(dur > 0) b->duration_us = (int64_t)(dur * (double)b->wm_timescale / 1000.0);
        break;
    }
    case ID_TRACKS: {
        const uint8_t *q = e.body, *qe = e.body + e.size;
        el_t c;
        while(q < qe && el_head(q, qe, &c) == 1 && c.size <= (uint64_t)(qe - c.body)){
            if(c.id == ID_TRACKENTRY) parse_track_entry(b, c.body, c.body + c.size);
            q = c.body + c.size;
        }
        if(!b->ntracks) return fail(b, "the WebM has no tracks");
        b->have_init = 1;
        break;
    }
    case ID_CLUSTER: {
        if(!b->have_init) return fail(b, "a media segment came before the init segment");
        b->wm_in_cluster = 0;
        mseg_t *seg = seg_new(b, e.body, (uint32_t)e.size);
        if(!seg) return fail(b, "out of memory (segment)");
        int rr = parse_cluster_kids(b, seg, seg->data, seg->data + seg->len);
        if(seg->refs == 0){ b->bytes_held -= seg->len; free(seg->data); free(seg); }
        if(rr != 0) return -1;
        break;
    }
    case ID_TIMECODE:
        b->wm_cluster_tc = (int64_t)ebml_uint(e.body, e.size);
        break;
    case ID_SIMPLEBLOCK: case ID_BLOCKGROUP: {
        if(!b->have_init) return fail(b, "a media segment came before the init segment");
        mseg_t *seg = seg_new(b, e.body, (uint32_t)e.size);
        if(!seg) return fail(b, "out of memory (block)");
        int rr = e.id == ID_SIMPLEBLOCK ? add_block(b, seg, seg->data, seg->data + seg->len, -1, 0)
                                        : add_group(b, seg, seg->data, seg->data + seg->len);
        if(seg->refs == 0){ b->bytes_held -= seg->len; free(seg->data); free(seg); }
        if(rr != 0) return -1;
        break;
    }
    }
    in_eat(b, (uint32_t)whole);
    return 1;
}

int sbuf_append(sbuf_t *b, const uint8_t *data, uint32_t len){
    if(b->error) return -1;
    if(len && in_add(b, data, len) != 0) return -1;
    for(;;){
        if(!b->fmt){
            if(b->in_len < 8) return 0;
            if(be32(b->in) == ID_EBML) b->fmt = 2;
            else if(!memcmp(b->in + 4, "ftyp", 4) || !memcmp(b->in + 4, "moov", 4) || !memcmp(b->in + 4, "styp", 4)
                    || !memcmp(b->in + 4, "moof", 4) || !memcmp(b->in + 4, "sidx", 4)) b->fmt = 1;
            else return fail(b, "neither MP4 nor WebM");
        }
        int r = b->fmt == 1 ? mp4_step(b) : webm_step(b);
        if(r < 0) return -1;
        if(r == 0) return 0;
    }
}
