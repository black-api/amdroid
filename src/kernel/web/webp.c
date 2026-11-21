/* webp.c — WebP pictures: lossless (VP8L), lossy (VP8, key frames), alpha (ALPH), and the
 * extended container (VP8X; an animation shows its first frame).
 *
 * VP8L: prefix codes (simple and normal), meta prefix codes, the color cache, LZ77 copies
 * with the 2-D distance map, and the four transforms (predictor, color, subtract green,
 * color indexing with pixel bundling).
 * VP8: the boolean decoder, segmentation, intra modes (16x16, 4x4, chroma), the DCT tokens
 * of every partition, dequantizing, the inverse WHT and DCT, the normal and simple loop
 * filters, and YUV 4:2:0 to RGB with libwebp's "fancy" chroma upsampling.
 * Constants of the VP8 format are in vp8_tables.h (RFC 6386).
 *
 * Output pixels are 0xTTRRGGBB with TT = 255 - alpha, like the other decoders here. */
#include "amdroid/types.h"
#include "amdroid/heap.h"
#include "string.h"
#include "vp8_tables.h"

#define WEBP_MAX_SIDE 8192
#define WEBP_MAX_PIX  (16u * 1024 * 1024 / 4)

static u32 rd24(const u8 *p){ return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16; }
static u32 rd32(const u8 *p){ return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u8 clip8(int v){ return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

/* ======================= VP8L (lossless) ======================= */

typedef struct { const u8 *p, *end; u64 val; int bits; int bad; } lbr_t;
static void lbr_fill(lbr_t *b){
    while(b->bits <= 56){
        if(b->p < b->end){ b->val |= (u64)*b->p++ << b->bits; }
        else if(b->bits < 0){ b->bad = 1; return; }
        else { /* past the end: zeros, and remember if they get used */ b->val |= 0; }
        b->bits += 8;
        if(b->p >= b->end && b->bits > 56) break;
    }
}
static u32 lbr_read(lbr_t *b, int n){
    if(n == 0) return 0;
    if(b->bits < n) lbr_fill(b);
    u32 v = (u32)(b->val & (((u64)1 << n) - 1));
    b->val >>= n; b->bits -= n;
    return v;
}

#define HC_FAST 8
typedef struct {
    u16 count[16], first[16], offset[16];
    u16 *sorted;                    /* the symbols by code */
    u16 fast[1 << HC_FAST];         /* next 8 bits -> symbol | length << 12 (0 = slow path) */
    int single;                     /* the one symbol of a 0-bit code, or -1 */
} hcode_t;

/* Builds the canonical code of the given lengths (0 = unused). 0 on success. */
static int hc_build(hcode_t *h, const u8 *lens, int n){
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    h->single = -1;
    int used = 0, last = 0;
    for(int i = 0; i < n; i++) if(lens[i]){ if(lens[i] > 15) return -1; h->count[lens[i]]++; used++; last = i; }
    h->sorted = kmalloc(sizeof(u16) * (u32)(used ? used : 1));
    if(!h->sorted) return -1;
    if(used == 0){ h->single = 0; return 0; }
    if(used == 1){ h->single = last; h->sorted[0] = (u16)last; return 0; }
    u16 off = 0;
    for(int l = 1; l < 16; l++){ h->offset[l] = off; off = (u16)(off + h->count[l]); }
    u16 fill[16];
    memcpy(fill, h->offset, sizeof fill);
    for(int i = 0; i < n; i++) if(lens[i]) h->sorted[fill[lens[i]]++] = (u16)i;
    u32 code = 0;
    for(int l = 1; l < 16; l++){
        code = (code + (l > 1 ? h->count[l - 1] : 0)) << 1;
        if(l == 1) code = 0;
        h->first[l] = (u16)code;
    }
    /* the fast table: codes of up to 8 bits, keyed by the stream's next bits (LSB first,
       so the code's bits are reversed) */
    for(int l = 1; l <= HC_FAST; l++)
        for(int k = 0; k < h->count[l]; k++){
            u32 c = h->first[l] + (u32)k, r = 0;
            for(int b = 0; b < l; b++) if(c & (1u << b)) r |= 1u << (l - 1 - b);
            u16 sym = h->sorted[h->offset[l] + k];
            for(u32 x = r; x < (1u << HC_FAST); x += 1u << l) h->fast[x] = (u16)(sym | (l << 12));
        }
    return 0;
}
static void hc_free(hcode_t *h){ if(h->sorted) kfree(h->sorted); h->sorted = NULL; }
static int hc_read(hcode_t *h, lbr_t *b){
    if(h->single >= 0) return h->single;
    if(b->bits < 16) lbr_fill(b);
    u16 e = h->fast[b->val & ((1u << HC_FAST) - 1)];
    if(e){ int l = e >> 12; b->val >>= l; b->bits -= l; return e & 0xFFF; }
    u32 code = 0;
    for(int l = 1; l < 16; l++){
        code = (code << 1) | (u32)(b->val & 1);
        b->val >>= 1; b->bits--;
        u32 d = code - h->first[l];
        if(code >= h->first[l] && d < h->count[l]) return h->sorted[h->offset[l] + d];
    }
    b->bad = 1;
    return 0;
}

static const u8 cl_order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* Reads one prefix code over an alphabet of n symbols. */
static int read_hcode(lbr_t *b, hcode_t *h, int n){
    u8 *lens = kmalloc((u32)n);
    if(!lens) return -1;
    memset(lens, 0, (u32)n);
    int r = 0;
    if(lbr_read(b, 1)){
        /* simple: one or two symbols */
        int ns = (int)lbr_read(b, 1) + 1;
        int first8 = (int)lbr_read(b, 1);
        int s0 = (int)lbr_read(b, first8 ? 8 : 1);
        if(s0 < n) lens[s0] = 1;
        if(ns == 2){ int s1 = (int)lbr_read(b, 8); if(s1 < n) lens[s1] = 1; }
        if(ns == 1){ r = hc_build(h, lens, n); h->single = s0 < n ? s0 : 0; kfree(lens); return r; }
    } else {
        u8 cll[19];
        memset(cll, 0, sizeof cll);
        int nc = 4 + (int)lbr_read(b, 4);
        if(nc > 19){ kfree(lens); return -1; }
        for(int i = 0; i < nc; i++) cll[cl_order[i]] = (u8)lbr_read(b, 3);
        hcode_t clc;
        memset(&clc, 0, sizeof clc);
        if(hc_build(&clc, cll, 19) != 0){ kfree(lens); return -1; }
        int max_sym = n;
        if(lbr_read(b, 1)){
            int lb = 2 + 2 * (int)lbr_read(b, 3);
            max_sym = 2 + (int)lbr_read(b, lb);
            if(max_sym > n){ hc_free(&clc); kfree(lens); return -1; }
        }
        int sym = 0, prev = 8;
        while(sym < n){
            if(max_sym-- == 0) break;
            int c = hc_read(&clc, b);
            if(b->bad) break;
            if(c < 16){ lens[sym++] = (u8)c; if(c) prev = c; continue; }
            int rep, val;
            if(c == 16){ rep = 3 + (int)lbr_read(b, 2); val = prev; }
            else if(c == 17){ rep = 3 + (int)lbr_read(b, 3); val = 0; }
            else { rep = 11 + (int)lbr_read(b, 7); val = 0; }
            if(sym + rep > n){ r = -1; break; }
            while(rep--) lens[sym++] = (u8)val;
        }
        hc_free(&clc);
    }
    if(r == 0) r = hc_build(h, lens, n);
    kfree(lens);
    return r;
}

static const i8 dist_map[120][2] = {
    {  0, 1 }, {  1, 0 }, {  1, 1 }, { -1, 1 }, {  0, 2 }, {  2, 0 }, {  1, 2 }, { -1, 2 },
    {  2, 1 }, { -2, 1 }, {  2, 2 }, { -2, 2 }, {  0, 3 }, {  3, 0 }, {  1, 3 }, { -1, 3 },
    {  3, 1 }, { -3, 1 }, {  2, 3 }, { -2, 3 }, {  3, 2 }, { -3, 2 }, {  0, 4 }, {  4, 0 },
    {  1, 4 }, { -1, 4 }, {  4, 1 }, { -4, 1 }, {  3, 3 }, { -3, 3 }, {  2, 4 }, { -2, 4 },
    {  4, 2 }, { -4, 2 }, {  0, 5 }, {  3, 4 }, { -3, 4 }, {  4, 3 }, { -4, 3 }, {  5, 0 },
    {  1, 5 }, { -1, 5 }, {  5, 1 }, { -5, 1 }, {  2, 5 }, { -2, 5 }, {  5, 2 }, { -5, 2 },
    {  4, 4 }, { -4, 4 }, {  3, 5 }, { -3, 5 }, {  5, 3 }, { -5, 3 }, {  0, 6 }, {  6, 0 },
    {  1, 6 }, { -1, 6 }, {  6, 1 }, { -6, 1 }, {  2, 6 }, { -2, 6 }, {  6, 2 }, { -6, 2 },
    {  4, 5 }, { -4, 5 }, {  5, 4 }, { -5, 4 }, {  3, 6 }, { -3, 6 }, {  6, 3 }, { -6, 3 },
    {  0, 7 }, {  7, 0 }, {  1, 7 }, { -1, 7 }, {  5, 5 }, { -5, 5 }, {  7, 1 }, { -7, 1 },
    {  4, 6 }, { -4, 6 }, {  6, 4 }, { -6, 4 }, {  2, 7 }, { -2, 7 }, {  7, 2 }, { -7, 2 },
    {  3, 7 }, { -3, 7 }, {  7, 3 }, { -7, 3 }, {  5, 6 }, { -5, 6 }, {  6, 5 }, { -6, 5 },
    {  8, 0 }, {  4, 7 }, { -4, 7 }, {  7, 4 }, { -7, 4 }, {  8, 1 }, {  8, 2 }, {  6, 6 },
    { -6, 6 }, {  8, 3 }, {  5, 7 }, { -5, 7 }, {  7, 5 }, { -7, 5 }, {  8, 4 }, {  6, 7 },
    { -6, 7 }, {  7, 6 }, { -7, 6 }, {  8, 5 }, {  7, 7 }, { -7, 7 }, {  8, 6 }, {  8, 7 }
};

static u32 prefix_value(lbr_t *b, int p){
    if(p < 4) return (u32)p + 1;
    int extra = (p - 2) >> 1;
    u32 off = (u32)(2 + (p & 1)) << extra;
    return off + lbr_read(b, extra) + 1;
}

/* An entropy-coded image of w x h ARGB pixels. level0: the main image (it may have meta
 * prefix codes). Returns kmalloc'd pixels or NULL. */
static u32 *vp8l_image(lbr_t *b, int w, int h, int level0){
    int cache_bits = 0;
    if(lbr_read(b, 1)){ cache_bits = (int)lbr_read(b, 4); if(cache_bits < 1 || cache_bits > 11) return NULL; }
    int meta_bits = 0, mw = 0;
    u32 *meta = NULL;
    int ngroups = 1;
    if(level0 && lbr_read(b, 1)){
        meta_bits = (int)lbr_read(b, 3) + 2;
        mw = (w + (1 << meta_bits) - 1) >> meta_bits;
        int mh = (h + (1 << meta_bits) - 1) >> meta_bits;
        meta = vp8l_image(b, mw, mh, 0);
        if(!meta) return NULL;
        for(int i = 0; i < mw * mh; i++){
            meta[i] = (meta[i] >> 8) & 0xFFFF;
            if((int)meta[i] + 1 > ngroups) ngroups = (int)meta[i] + 1;
        }
        if(ngroups > 4096){ kfree(meta); return NULL; }
    }
    int csize = cache_bits ? 1 << cache_bits : 0;
    hcode_t *g = kmalloc(sizeof(hcode_t) * 5 * (u32)ngroups);
    u32 *px = (u64)w * h <= WEBP_MAX_PIX ? kmalloc(4u * (u32)w * (u32)h) : NULL;
    u32 *cache = csize ? kmalloc(4u * (u32)csize) : NULL;
    int ok = g && px && (!csize || cache);
    if(g) memset(g, 0, sizeof(hcode_t) * 5 * (u32)ngroups);
    if(cache) memset(cache, 0, 4u * (u32)csize);
    static const int asize[5] = { 280, 256, 256, 256, 40 };
    for(int i = 0; ok && i < ngroups * 5; i++){
        int n = asize[i % 5] + (i % 5 == 0 ? csize : 0);
        if(read_hcode(b, &g[i], n) != 0 || b->bad) ok = 0;
    }
    int total = w * h, pos = 0;
    while(ok && pos < total){
        int x = pos % w, y = pos / w;
        hcode_t *gr = g;
        if(meta) gr = g + 5 * meta[(y >> meta_bits) * mw + (x >> meta_bits)];
        int s = hc_read(&gr[0], b);
        if(b->bad){ ok = 0; break; }
        if(s < 256){
            int r = hc_read(&gr[1], b), bl = hc_read(&gr[2], b), a = hc_read(&gr[3], b);
            u32 argb = (u32)a << 24 | (u32)r << 16 | (u32)s << 8 | (u32)bl;
            px[pos++] = argb;
            if(csize) cache[(0x1e35a7bdu * argb) >> (32 - cache_bits)] = argb;
        } else if(s < 256 + 24){
            u32 len = prefix_value(b, s - 256);
            int dc = hc_read(&gr[4], b);
            u32 dcode = prefix_value(b, dc);
            int dist;
            if(dcode > 120) dist = (int)dcode - 120;
            else {
                dist = dist_map[dcode - 1][0] + dist_map[dcode - 1][1] * w;
                if(dist < 1) dist = 1;
            }
            if(dist > pos){ ok = 0; break; }
            if((u32)(total - pos) < len) len = (u32)(total - pos);
            for(u32 k = 0; k < len; k++){
                u32 argb = px[pos - dist];
                px[pos++] = argb;
                if(csize) cache[(0x1e35a7bdu * argb) >> (32 - cache_bits)] = argb;
            }
        } else {
            int ci = s - 280;
            if(ci >= csize){ ok = 0; break; }
            u32 argb = cache[ci];
            px[pos++] = argb;
            cache[(0x1e35a7bdu * argb) >> (32 - cache_bits)] = argb;
        }
        if(b->bad) ok = 0;
    }
    if(g){ for(int i = 0; i < ngroups * 5; i++) hc_free(&g[i]); kfree(g); }
    if(cache) kfree(cache);
    if(meta) kfree(meta);
    if(!ok){ if(px) kfree(px); return NULL; }
    return px;
}

static inline u32 avg2(u32 a, u32 b){ return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }
static inline int ch(u32 p, int s){ return (int)((p >> s) & 255); }
static u32 sel(u32 L, u32 T, u32 TL){
    int pl = 0, pt = 0;
    for(int s = 0; s < 32; s += 8){
        int p = ch(L, s) + ch(T, s) - ch(TL, s);
        pl += p - ch(L, s) < 0 ? ch(L, s) - p : p - ch(L, s);
        pt += p - ch(T, s) < 0 ? ch(T, s) - p : p - ch(T, s);
    }
    return pl < pt ? L : T;
}
static u32 clamp_full(u32 a, u32 b, u32 c){
    u32 r = 0;
    for(int s = 0; s < 32; s += 8) r |= (u32)clip8(ch(a, s) + ch(b, s) - ch(c, s)) << s;
    return r;
}
static u32 clamp_half(u32 a, u32 b){
    u32 r = 0;
    for(int s = 0; s < 32; s += 8){ int x = ch(a, s); r |= (u32)clip8(x + (x - ch(b, s)) / 2) << s; }
    return r;
}
static u32 add_px(u32 a, u32 b){
    return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) | (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu);
}

typedef struct { int type, bits, xs; u32 *data; int ncolors; } xform_t;

/* The pixels of a VP8L stream (after its 5-byte header, or an ALPH chunk's data). */
static u32 *vp8l_decode_stream(lbr_t *b, int w, int h){
    xform_t xf[4]; int nx = 0;
    int xs = w;
    int seen = 0;
    u32 *px = NULL;
    while(lbr_read(b, 1)){
        int t = (int)lbr_read(b, 2);
        if((seen >> t) & 1 || nx >= 4) goto fail;
        seen |= 1 << t;
        xform_t *x = &xf[nx++];
        memset(x, 0, sizeof *x);
        x->type = t; x->xs = xs;
        if(t == 0 || t == 1){
            x->bits = (int)lbr_read(b, 3) + 2;
            int bw = (xs + (1 << x->bits) - 1) >> x->bits, bh = (h + (1 << x->bits) - 1) >> x->bits;
            x->data = vp8l_image(b, bw, bh, 0);
            if(!x->data) goto fail;
        } else if(t == 3){
            int n = (int)lbr_read(b, 8) + 1;
            x->ncolors = n;
            u32 *pal = vp8l_image(b, n, 1, 0);
            if(!pal) goto fail;
            x->data = kmalloc(4 * 256);
            if(!x->data){ kfree(pal); goto fail; }
            memset(x->data, 0, 4 * 256);
            for(int i = 0; i < n; i++) x->data[i] = i ? add_px(pal[i], x->data[i - 1]) : pal[i];
            kfree(pal);
            x->bits = n <= 2 ? 3 : n <= 4 ? 2 : n <= 16 ? 1 : 0;
            xs = (xs + (1 << x->bits) - 1) >> x->bits;
        }
    }
    px = vp8l_image(b, xs, h, 1);
    if(!px) goto fail;
    /* the transforms, last read first */
    for(int k = nx - 1; k >= 0; k--){
        xform_t *x = &xf[k];
        int W = x->xs;
        if(x->type == 2){
            for(int i = 0; i < W * h; i++){
                u32 p = px[i], g = (p >> 8) & 255;
                px[i] = (p & 0xFF00FF00u) | ((((p >> 16) + g) & 255) << 16) | (((p & 255) + g) & 255);
            }
        } else if(x->type == 1){
            int bw = (W + (1 << x->bits) - 1) >> x->bits;
            for(int y = 0; y < h; y++) for(int xx = 0; xx < W; xx++){
                u32 m = x->data[(y >> x->bits) * bw + (xx >> x->bits)];
                i8 g2r = (i8)(m & 255), g2b = (i8)((m >> 8) & 255), r2b = (i8)((m >> 16) & 255);
                u32 p = px[y * W + xx];
                i8 g = (i8)((p >> 8) & 255);
                int r = (int)((p >> 16) & 255) + ((g2r * g) >> 5);
                int bl = (int)(p & 255) + ((g2b * g) >> 5);
                bl += (r2b * (i8)(r & 255)) >> 5;
                px[y * W + xx] = (p & 0xFF00FF00u) | ((u32)(r & 255) << 16) | (u32)(bl & 255);
            }
        } else if(x->type == 0){
            int bw = (W + (1 << x->bits) - 1) >> x->bits;
            for(int y = 0; y < h; y++) for(int xx = 0; xx < W; xx++){
                int i = y * W + xx;
                u32 pred;
                if(y == 0) pred = xx == 0 ? 0xFF000000u : px[i - 1];
                else if(xx == 0) pred = px[i - W];
                else {
                    u32 L = px[i - 1], T = px[i - W], TL = px[i - W - 1], TR = px[i - W + 1];
                    switch((x->data[(y >> x->bits) * bw + (xx >> x->bits)] >> 8) & 15){
                    case 0: pred = 0xFF000000u; break;
                    case 1: pred = L; break;
                    case 2: pred = T; break;
                    case 3: pred = TR; break;
                    case 4: pred = TL; break;
                    case 5: pred = avg2(avg2(L, TR), T); break;
                    case 6: pred = avg2(L, TL); break;
                    case 7: pred = avg2(L, T); break;
                    case 8: pred = avg2(TL, T); break;
                    case 9: pred = avg2(T, TR); break;
                    case 10: pred = avg2(avg2(L, TL), avg2(T, TR)); break;
                    case 11: pred = sel(L, T, TL); break;
                    case 12: pred = clamp_full(L, T, TL); break;
                    case 13: pred = clamp_half(avg2(L, T), TL); break;
                    default: pred = 0xFF000000u;
                    }
                }
                px[i] = add_px(px[i], pred);
            }
        } else {
            /* color indexing: unbundle the packed indices, then look each up */
            int packed = (W + (1 << x->bits) - 1) >> x->bits;
            u32 *out = kmalloc(4u * (u32)W * (u32)h);
            if(!out) goto fail;
            int per = 1 << x->bits, ib = 8 >> x->bits, mask = (1 << ib) - 1;
            for(int y = 0; y < h; y++) for(int xx = 0; xx < W; xx++){
                u32 g = (px[y * packed + xx / per] >> 8) & 255;
                int idx = (int)(g >> ((xx % per) * ib)) & mask;
                out[y * W + xx] = x->data[idx & 255];
            }
            kfree(px);
            px = out;
        }
    }
    for(int k = 0; k < nx; k++) if(xf[k].data) kfree(xf[k].data);
    return px;
fail:
    for(int k = 0; k < nx; k++) if(xf[k].data) kfree(xf[k].data);
    if(px) kfree(px);
    return NULL;
}

/* ======================= VP8 (lossy) ======================= */

typedef struct { const u8 *p, *end; u32 value, range; int count; } bd_t;
static void bd_init(bd_t *d, const u8 *p, const u8 *end){
    d->p = p; d->end = end; d->value = 0; d->count = 0; d->range = 255;
    for(int i = 0; i < 2; i++) d->value = (d->value << 8) | (d->p < d->end ? *d->p++ : 0);
}
static inline int bd_get(bd_t *d, int prob){
    u32 split = 1 + (((d->range - 1) * (u32)prob) >> 8);
    u32 S = split << 8;
    int r;
    if(d->value >= S){ r = 1; d->range -= split; d->value -= S; }
    else { r = 0; d->range = split; }
    while(d->range < 128){
        d->value <<= 1; d->range <<= 1;
        if(++d->count == 8){ d->count = 0; d->value |= d->p < d->end ? *d->p++ : 0; }
    }
    return r;
}
static u32 bd_lit(bd_t *d, int n){ u32 v = 0; while(n--) v = (v << 1) | (u32)bd_get(d, 128); return v; }
static int bd_signed(bd_t *d, int n){ int v = (int)bd_lit(d, n); return bd_get(d, 128) ? -v : v; }
static int bd_tree(bd_t *d, const i8 (*tree)[2], const u8 *probs){
    int i = 0;
    do i = tree[i][bd_get(d, probs[i])]; while(i > 0);
    return -i;
}

/* the mode trees, in FFmpeg's numbering (see vp8_tables.h) */
enum { M_DC = 0, M_HOR = 1, M_VERT = 2, M_TM = 3, M_SPLIT = 4 };                    /* 16x16, chroma */
enum { B_VE = 0, B_HE = 1, B_DC = 2, B_LD = 3, B_RD = 4, B_VR = 5, B_HD = 6, B_VL = 7, B_HU = 8, B_TM = 9 };
static const i8 ymode_tree[4][2] = { { -M_SPLIT, 1 }, { 2, 3 }, { -M_DC, -M_VERT }, { -M_HOR, -M_TM } };
static const i8 uvmode_tree[3][2] = { { -M_DC, 1 }, { -M_VERT, 2 }, { -M_HOR, -M_TM } };
static const i8 bmode_tree[9][2] = {
    { -B_DC, 1 }, { -B_TM, 2 }, { -B_VE, 3 }, { 4, 6 }, { -B_HE, 5 }, { -B_RD, -B_VR },
    { -B_LD, 7 }, { -B_VL, 8 }, { -B_HD, -B_HU }
};
static const u8 ymode_to_b[4] = { B_DC, B_HE, B_VE, B_TM };
static const u8 zigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

typedef struct {
    int W, H, mbw, mbh;
    u8 *y, *u, *v;            /* planes with a 1-pixel border above and to the left */
    int ys, uvs;              /* strides */
    u8 coef[4][8][3][11];
    int seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    u8 seg_prob[3];
    int ftype, flevel, sharp;
    int lf_adj, ref_delta[4], mode_delta[4];
    int skip_on, skip_prob;
    int dq[4][3][2];          /* per segment: [y, y2, uv][dc, ac] */
    u8 *lf_level, *lf_inner;  /* per macroblock */
} vp8_t;

/* One block's tokens; returns 0 if it has none, else the index after the last one. */
static int read_coeffs(bd_t *d, vp8_t *v, int type, int ctx, int first, const int dq[2], short out[16]){
    int i = first;
    const u8 *p = v->coef[type][vp8_band[i]][ctx];
    if(!bd_get(d, p[0])) return 0;
    for(;;){
        while(!bd_get(d, p[1])){
            if(++i == 16) return 16;
            p = v->coef[type][vp8_band[i]][0];
        }
        int val, nctx;
        if(!bd_get(d, p[2])){ val = 1; nctx = 1; }
        else {
            nctx = 2;
            if(!bd_get(d, p[3])){
                if(!bd_get(d, p[4])) val = 2;
                else val = 3 + bd_get(d, p[5]);
            } else if(!bd_get(d, p[6])){
                if(!bd_get(d, p[7])) val = 5 + bd_get(d, vp8_cat1[0]);
                else { val = 7 + 2 * bd_get(d, vp8_cat2[0]); val += bd_get(d, vp8_cat2[1]); }
            } else {
                int b1 = bd_get(d, p[8]);
                int b0 = bd_get(d, p[9 + b1]);
                int cat = 2 * b1 + b0;
                static const u8 *const cats[4] = { vp8_cat3, vp8_cat4, vp8_cat5, vp8_cat6 };
                static const int base[4] = { 11, 19, 35, 67 };
                int e = 0;
                for(const u8 *c = cats[cat]; *c; c++) e = (e << 1) | bd_get(d, *c);
                val = base[cat] + e;
            }
        }
        if(bd_get(d, 128)) val = -val;
        out[zigzag[i]] = (short)(val * dq[i > 0]);
        if(++i == 16) return 16;
        p = v->coef[type][vp8_band[i]][nctx];
        if(!bd_get(d, p[0])) return i;
    }
}

static void iwht(const short in[16], short out[16][16]){
    int t[16];
    for(int i = 0; i < 4; i++){
        int a1 = in[i] + in[12 + i], b1 = in[4 + i] + in[8 + i];
        int c1 = in[4 + i] - in[8 + i], d1 = in[i] - in[12 + i];
        t[i] = a1 + b1; t[4 + i] = c1 + d1; t[8 + i] = a1 - b1; t[12 + i] = d1 - c1;
    }
    for(int i = 0; i < 4; i++){
        int a1 = t[4 * i] + t[4 * i + 3], b1 = t[4 * i + 1] + t[4 * i + 2];
        int c1 = t[4 * i + 1] - t[4 * i + 2], d1 = t[4 * i] - t[4 * i + 3];
        out[4 * i][0] = (short)((a1 + b1 + 3) >> 3);
        out[4 * i + 1][0] = (short)((c1 + d1 + 3) >> 3);
        out[4 * i + 2][0] = (short)((a1 - b1 + 3) >> 3);
        out[4 * i + 3][0] = (short)((d1 - c1 + 3) >> 3);
    }
}
/* adds the inverse DCT of in to the 4x4 pixels at dst */
static void idct_add(const short in[16], u8 *dst, int stride){
    int t[16];
    for(int i = 0; i < 4; i++){
        int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
        int c1 = ((in[4 + i] * 35468) >> 16) - (in[12 + i] + ((in[12 + i] * 20091) >> 16));
        int d1 = (in[4 + i] + ((in[4 + i] * 20091) >> 16)) + ((in[12 + i] * 35468) >> 16);
        t[i] = a1 + d1; t[12 + i] = a1 - d1; t[4 + i] = b1 + c1; t[8 + i] = b1 - c1;
    }
    for(int r = 0; r < 4; r++){
        const int *ip = t + 4 * r;
        int a1 = ip[0] + ip[2], b1 = ip[0] - ip[2];
        int c1 = ((ip[1] * 35468) >> 16) - (ip[3] + ((ip[3] * 20091) >> 16));
        int d1 = (ip[1] + ((ip[1] * 20091) >> 16)) + ((ip[3] * 35468) >> 16);
        u8 *o = dst + r * stride;
        o[0] = clip8(o[0] + ((a1 + d1 + 4) >> 3));
        o[3] = clip8(o[3] + ((a1 - d1 + 4) >> 3));
        o[1] = clip8(o[1] + ((b1 + c1 + 4) >> 3));
        o[2] = clip8(o[2] + ((b1 - c1 + 4) >> 3));
    }
}

/* 16x16 / 8x8 prediction into dst (the plane, at the block). top/left: whether they exist. */
static void pred_block(u8 *dst, int stride, int n, int mode, int top, int left){
    const u8 *A = dst - stride;
    switch(mode){
    case M_DC: {
        int s = 0, cnt = 0, shift = n == 16 ? 3 : 2;
        if(top){ for(int i = 0; i < n; i++) s += A[i]; cnt++; }
        if(left){ for(int i = 0; i < n; i++) s += dst[i * stride - 1]; cnt++; }
        int dc = cnt == 2 ? (s + n) >> (shift + 2) : cnt == 1 ? (s + n / 2) >> (shift + 1) : 128;
        for(int y = 0; y < n; y++) memset(dst + y * stride, dc, (u32)n);
        break; }
    case M_VERT: for(int y = 0; y < n; y++) memcpy(dst + y * stride, A, (u32)n); break;
    case M_HOR: for(int y = 0; y < n; y++) memset(dst + y * stride, dst[y * stride - 1], (u32)n); break;
    default: {
        int P = A[-1];
        for(int y = 0; y < n; y++){ int L = dst[y * stride - 1]; for(int x = 0; x < n; x++) dst[y * stride + x] = clip8(L + A[x] - P); }
    }
    }
}
#define AV2(a, b) (((a) + (b) + 1) >> 1)
#define AV3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
/* a 4x4 block's prediction; A[-1..7] above (A[-1] the corner), L[0..3] left */
static void pred4(u8 *dst, int s, int mode, const u8 *A, const u8 *L){
    u8 B[4][4];
    int P = A[-1];
    int E[9] = { L[3], L[2], L[1], L[0], P, A[0], A[1], A[2], A[3] };
    switch(mode){
    case B_DC: { int v = 4; for(int i = 0; i < 4; i++) v += A[i] + L[i]; v >>= 3; for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++) B[r][c] = (u8)v; break; }
    case B_TM: for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++) B[r][c] = clip8(L[r] + A[c] - P); break;
    case B_VE: for(int c = 0; c < 4; c++){ int v = AV3(A[c - 1], A[c], A[c + 1]); for(int r = 0; r < 4; r++) B[r][c] = (u8)v; } break;
    case B_HE: {
        int v0 = AV3(P, L[0], L[1]), v1 = AV3(L[0], L[1], L[2]), v2 = AV3(L[1], L[2], L[3]), v3 = AV3(L[2], L[3], L[3]);
        for(int c = 0; c < 4; c++){ B[0][c] = (u8)v0; B[1][c] = (u8)v1; B[2][c] = (u8)v2; B[3][c] = (u8)v3; }
        break; }
    case B_LD:
        B[0][0] = AV3(A[0], A[1], A[2]);
        B[0][1] = B[1][0] = AV3(A[1], A[2], A[3]);
        B[0][2] = B[1][1] = B[2][0] = AV3(A[2], A[3], A[4]);
        B[0][3] = B[1][2] = B[2][1] = B[3][0] = AV3(A[3], A[4], A[5]);
        B[1][3] = B[2][2] = B[3][1] = AV3(A[4], A[5], A[6]);
        B[2][3] = B[3][2] = AV3(A[5], A[6], A[7]);
        B[3][3] = AV3(A[6], A[7], A[7]);
        break;
    case B_RD:
        B[3][0] = AV3(E[0], E[1], E[2]);
        B[3][1] = B[2][0] = AV3(E[1], E[2], E[3]);
        B[3][2] = B[2][1] = B[1][0] = AV3(E[2], E[3], E[4]);
        B[3][3] = B[2][2] = B[1][1] = B[0][0] = AV3(E[3], E[4], E[5]);
        B[2][3] = B[1][2] = B[0][1] = AV3(E[4], E[5], E[6]);
        B[1][3] = B[0][2] = AV3(E[5], E[6], E[7]);
        B[0][3] = AV3(E[6], E[7], E[8]);
        break;
    case B_VR:
        B[3][0] = AV3(E[1], E[2], E[3]);
        B[2][0] = AV3(E[2], E[3], E[4]);
        B[3][1] = B[1][0] = AV3(E[3], E[4], E[5]);
        B[2][1] = B[0][0] = AV2(E[4], E[5]);
        B[3][2] = B[1][1] = AV3(E[4], E[5], E[6]);
        B[2][2] = B[0][1] = AV2(E[5], E[6]);
        B[3][3] = B[1][2] = AV3(E[5], E[6], E[7]);
        B[2][3] = B[0][2] = AV2(E[6], E[7]);
        B[1][3] = AV3(E[6], E[7], E[8]);
        B[0][3] = AV2(E[7], E[8]);
        break;
    case B_VL:
        B[0][0] = AV2(A[0], A[1]);
        B[1][0] = AV3(A[0], A[1], A[2]);
        B[2][0] = B[0][1] = AV2(A[1], A[2]);
        B[1][1] = B[3][0] = AV3(A[1], A[2], A[3]);
        B[2][1] = B[0][2] = AV2(A[2], A[3]);
        B[3][1] = B[1][2] = AV3(A[2], A[3], A[4]);
        B[2][2] = B[0][3] = AV2(A[3], A[4]);
        B[3][2] = B[1][3] = AV3(A[3], A[4], A[5]);
        B[2][3] = AV3(A[4], A[5], A[6]);
        B[3][3] = AV3(A[5], A[6], A[7]);
        break;
    case B_HD:
        B[3][0] = AV2(E[0], E[1]);
        B[3][1] = AV3(E[0], E[1], E[2]);
        B[2][0] = B[3][2] = AV2(E[1], E[2]);
        B[2][1] = B[3][3] = AV3(E[1], E[2], E[3]);
        B[2][2] = B[1][0] = AV2(E[2], E[3]);
        B[2][3] = B[1][1] = AV3(E[2], E[3], E[4]);
        B[1][2] = B[0][0] = AV2(E[3], E[4]);
        B[1][3] = B[0][1] = AV3(E[3], E[4], E[5]);
        B[0][2] = AV3(E[4], E[5], E[6]);
        B[0][3] = AV3(E[5], E[6], E[7]);
        break;
    default: /* B_HU */
        B[0][0] = AV2(L[0], L[1]);
        B[0][1] = AV3(L[0], L[1], L[2]);
        B[0][2] = B[1][0] = AV2(L[1], L[2]);
        B[0][3] = B[1][1] = AV3(L[1], L[2], L[3]);
        B[1][2] = B[2][0] = AV2(L[2], L[3]);
        B[1][3] = B[2][1] = AV3(L[2], L[3], L[3]);
        B[2][2] = B[2][3] = B[3][0] = B[3][1] = B[3][2] = B[3][3] = (u8)L[3];
    }
    for(int r = 0; r < 4; r++) memcpy(dst + r * s, B[r], 4);
}

/* ---- the loop filter ---- */
static inline int s8c(int v){ return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int u2s(int v){ return v - 128; }
static inline u8 s2u(int v){ return (u8)(s8c(v) + 128); }
static inline int iabs(int v){ return v < 0 ? -v : v; }
/* one edge position: p[-4*st..3*st] across the edge */
static inline int common_adjust(int outer, u8 *P1, u8 *P0, u8 *Q0, u8 *Q1){
    int p1 = u2s(*P1), p0 = u2s(*P0), q0 = u2s(*Q0), q1 = u2s(*Q1);
    int a = s8c((outer ? s8c(p1 - q1) : 0) + 3 * (q0 - p0));
    int b = s8c(a + 3) >> 3;
    a = s8c(a + 4) >> 3;
    *Q0 = s2u(q0 - a); *P0 = s2u(p0 + b);
    return a;
}
static void filt_simple(u8 *p, int st, int lim, int n, int along){
    for(int i = 0; i < n; i++, p += along){
        if(iabs(p[-st] - p[0]) * 2 + iabs(p[-2 * st] - p[st]) / 2 <= lim) common_adjust(1, p - 2 * st, p - st, p, p + st);
    }
}
static void filt_normal(u8 *p, int st, int E, int I, int hev, int n, int along, int mb){
    for(int i = 0; i < n; i++, p += along){
        int p3 = p[-4 * st], p2 = p[-3 * st], p1 = p[-2 * st], p0 = p[-st];
        int q0 = p[0], q1 = p[st], q2 = p[2 * st], q3 = p[3 * st];
        if(iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > E) continue;
        if(iabs(p3 - p2) > I || iabs(p2 - p1) > I || iabs(p1 - p0) > I || iabs(q3 - q2) > I || iabs(q2 - q1) > I || iabs(q1 - q0) > I) continue;
        int hv = iabs(p1 - p0) > hev || iabs(q1 - q0) > hev;
        if(mb){
            if(hv){ common_adjust(1, p - 2 * st, p - st, p, p + st); continue; }
            int P2 = u2s(p2), P1 = u2s(p1), P0 = u2s(p0), Q0 = u2s(q0), Q1 = u2s(q1), Q2 = u2s(q2);
            int w = s8c(s8c(P1 - Q1) + 3 * (Q0 - P0));
            int a = s8c((27 * w + 63) >> 7);
            p[0] = s2u(Q0 - a); p[-st] = s2u(P0 + a);
            a = s8c((18 * w + 63) >> 7);
            p[st] = s2u(Q1 - a); p[-2 * st] = s2u(P1 + a);
            a = s8c((9 * w + 63) >> 7);
            p[2 * st] = s2u(Q2 - a); p[-3 * st] = s2u(P2 + a);
        } else {
            int a = (common_adjust(hv, p - 2 * st, p - st, p, p + st) + 1) >> 1;
            if(!hv){ p[st] = s2u(u2s(q1) - a); p[-2 * st] = s2u(u2s(p1) + a); }
        }
    }
}
static void loop_filter(vp8_t *v){
    for(int my = 0; my < v->mbh; my++) for(int mx = 0; mx < v->mbw; mx++){
        int lvl = v->lf_level[my * v->mbw + mx];
        if(!lvl) continue;
        int inner = v->lf_inner[my * v->mbw + mx];
        int I = lvl;
        if(v->sharp){ I >>= v->sharp > 4 ? 2 : 1; if(I > 9 - v->sharp) I = 9 - v->sharp; }
        if(I < 1) I = 1;
        int hev = lvl >= 40 ? 2 : lvl >= 15 ? 1 : 0;
        int mbE = (lvl + 2) * 2 + I, subE = lvl * 2 + I;
        u8 *Y = v->y + (my * 16) * v->ys + mx * 16;
        u8 *U = v->u + (my * 8) * v->uvs + mx * 8, *V = v->v + (my * 8) * v->uvs + mx * 8;
        if(v->ftype){
            if(mx > 0) filt_simple(Y, 1, mbE, 16, v->ys);
            if(inner) for(int k = 4; k < 16; k += 4) filt_simple(Y + k, 1, subE, 16, v->ys);
            if(my > 0) filt_simple(Y, v->ys, mbE, 16, 1);
            if(inner) for(int k = 4; k < 16; k += 4) filt_simple(Y + k * v->ys, v->ys, subE, 16, 1);
            continue;
        }
        if(mx > 0){
            filt_normal(Y, 1, mbE, I, hev, 16, v->ys, 1);
            filt_normal(U, 1, mbE, I, hev, 8, v->uvs, 1);
            filt_normal(V, 1, mbE, I, hev, 8, v->uvs, 1);
        }
        if(inner){
            for(int k = 4; k < 16; k += 4) filt_normal(Y + k, 1, subE, I, hev, 16, v->ys, 0);
            filt_normal(U + 4, 1, subE, I, hev, 8, v->uvs, 0);
            filt_normal(V + 4, 1, subE, I, hev, 8, v->uvs, 0);
        }
        if(my > 0){
            filt_normal(Y, v->ys, mbE, I, hev, 16, 1, 1);
            filt_normal(U, v->uvs, mbE, I, hev, 8, 1, 1);
            filt_normal(V, v->uvs, mbE, I, hev, 8, 1, 1);
        }
        if(inner){
            for(int k = 4; k < 16; k += 4) filt_normal(Y + k * v->ys, v->ys, subE, I, hev, 16, 1, 0);
            filt_normal(U + 4 * v->uvs, v->uvs, subE, I, hev, 8, 1, 0);
            filt_normal(V + 4 * v->uvs, v->uvs, subE, I, hev, 8, 1, 0);
        }
    }
}

static void vp8_free(vp8_t *v){
    if(v->y) kfree(v->y - v->ys - 1);
    if(v->u) kfree(v->u - v->uvs - 1);
    if(v->v) kfree(v->v - v->uvs - 1);
    if(v->lf_level) kfree(v->lf_level);
    if(v->lf_inner) kfree(v->lf_inner);
}

/* Decodes a VP8 key frame into v's Y, U and V planes. 0 on success. */
static int vp8_decode(vp8_t *v, const u8 *d, u32 len){
    memset(v, 0, sizeof *v);
    if(len < 10) return -1;
    u32 tag = rd24(d);
    if(tag & 1) return -1;                                  /* not a key frame */
    u32 p0len = tag >> 5;
    if(d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return -1;
    v->W = (d[6] | d[7] << 8) & 0x3FFF; v->H = (d[8] | d[9] << 8) & 0x3FFF;
    if(!v->W || !v->H || v->W > WEBP_MAX_SIDE || v->H > WEBP_MAX_SIDE) return -1;
    if((u64)v->W * v->H > WEBP_MAX_PIX) return -1;
    const u8 *end = d + len, *pp = d + 10;
    if(pp + p0len > end) return -1;
    v->mbw = (v->W + 15) / 16; v->mbh = (v->H + 15) / 16;
    v->ys = v->mbw * 16 + 1 + 4; v->uvs = v->mbw * 8 + 1 + 4;
    u8 *yb = kmalloc((u32)v->ys * (u32)(v->mbh * 16 + 1));
    u8 *ub = kmalloc((u32)v->uvs * (u32)(v->mbh * 8 + 1));
    u8 *vb = kmalloc((u32)v->uvs * (u32)(v->mbh * 8 + 1));
    v->lf_level = kmalloc((u32)(v->mbw * v->mbh));
    v->lf_inner = kmalloc((u32)(v->mbw * v->mbh));
    if(yb){ v->y = yb + v->ys + 1; } if(ub){ v->u = ub + v->uvs + 1; } if(vb){ v->v = vb + v->uvs + 1; }
    if(!yb || !ub || !vb || !v->lf_level || !v->lf_inner) return -1;
    /* the borders: 127 above (and the corner), 129 to the left */
    memset(yb, 127, (u32)v->ys);
    memset(ub, 127, (u32)v->uvs); memset(vb, 127, (u32)v->uvs);
    for(int y = 0; y < v->mbh * 16; y++) v->y[y * v->ys - 1] = 129;
    for(int y = 0; y < v->mbh * 8; y++){ v->u[y * v->uvs - 1] = 129; v->v[y * v->uvs - 1] = 129; }

    bd_t bd;
    bd_init(&bd, pp, pp + p0len);
    bd_get(&bd, 128); bd_get(&bd, 128);                     /* colour space, clamping */
    v->seg_on = bd_get(&bd, 128);
    if(v->seg_on){
        v->seg_map = bd_get(&bd, 128);
        int upd = bd_get(&bd, 128);
        if(upd){
            v->seg_abs = bd_get(&bd, 128);
            for(int i = 0; i < 4; i++) v->seg_q[i] = bd_get(&bd, 128) ? bd_signed(&bd, 7) : 0;
            for(int i = 0; i < 4; i++) v->seg_lf[i] = bd_get(&bd, 128) ? bd_signed(&bd, 6) : 0;
        }
        if(v->seg_map) for(int i = 0; i < 3; i++) v->seg_prob[i] = bd_get(&bd, 128) ? (u8)bd_lit(&bd, 8) : 255;
    }
    v->ftype = bd_get(&bd, 128);
    v->flevel = (int)bd_lit(&bd, 6);
    v->sharp = (int)bd_lit(&bd, 3);
    v->lf_adj = bd_get(&bd, 128);
    if(v->lf_adj && bd_get(&bd, 128)){
        for(int i = 0; i < 4; i++) if(bd_get(&bd, 128)) v->ref_delta[i] = bd_signed(&bd, 6);
        for(int i = 0; i < 4; i++) if(bd_get(&bd, 128)) v->mode_delta[i] = bd_signed(&bd, 6);
    }
    int nparts = 1 << bd_lit(&bd, 2);
    int qi = (int)bd_lit(&bd, 7);
    int dlt[5];
    for(int i = 0; i < 5; i++) dlt[i] = bd_get(&bd, 128) ? bd_signed(&bd, 4) : 0;    /* ydc y2dc y2ac uvdc uvac */
    bd_get(&bd, 128);                                       /* refresh entropy probs */
    memcpy(v->coef, vp8_coef_default, sizeof v->coef);
    for(int i = 0; i < 4; i++) for(int j = 0; j < 8; j++) for(int k = 0; k < 3; k++) for(int l = 0; l < 11; l++)
        if(bd_get(&bd, vp8_coef_update[i][j][k][l])) v->coef[i][j][k][l] = (u8)bd_lit(&bd, 8);
    v->skip_on = bd_get(&bd, 128);
    if(v->skip_on) v->skip_prob = (int)bd_lit(&bd, 8);
    /* quantizer steps per segment */
    for(int s = 0; s < 4; s++){
        int q = qi;
        if(v->seg_on) q = v->seg_abs ? v->seg_q[s] : qi + v->seg_q[s];
        #define QC(x) ((x) < 0 ? 0 : (x) > 127 ? 127 : (x))
        v->dq[s][0][0] = vp8_dc_q[QC(q + dlt[0])];
        v->dq[s][0][1] = vp8_ac_q[QC(q)];
        v->dq[s][1][0] = vp8_dc_q[QC(q + dlt[1])] * 2;
        v->dq[s][1][1] = vp8_ac_q[QC(q + dlt[2])] * 155 / 100;
        if(v->dq[s][1][1] < 8) v->dq[s][1][1] = 8;
        v->dq[s][2][0] = vp8_dc_q[QC(q + dlt[3])];
        if(v->dq[s][2][0] > 132) v->dq[s][2][0] = 132;
        v->dq[s][2][1] = vp8_ac_q[QC(q + dlt[4])];
    }
    /* the token partitions */
    bd_t parts[8];
    const u8 *ps = pp + p0len;
    const u8 *pdata = ps + 3 * (nparts - 1);
    if(pdata > end) return -1;
    for(int i = 0; i < nparts; i++){
        u32 sz = i < nparts - 1 ? rd24(ps + 3 * i) : (u32)(end - pdata);
        if(pdata + sz > end) sz = (u32)(end - pdata);
        bd_init(&parts[i], pdata, pdata + sz);
        pdata += sz;
    }

    u8 *top_b = kmalloc((u32)v->mbw * 4);                   /* 4x4 mode contexts above */
    u8 *top_nz = kmalloc((u32)v->mbw * 9);                  /* non-zero contexts above: 4 Y, 2 U, 2 V, Y2 */
    if(!top_b || !top_nz){ if(top_b) kfree(top_b); if(top_nz) kfree(top_nz); return -1; }
    memset(top_b, B_DC, (u32)v->mbw * 4);
    memset(top_nz, 0, (u32)v->mbw * 9);
    static short blk[25][16];
    for(int my = 0; my < v->mbh; my++){
        u8 left_b[4] = { B_DC, B_DC, B_DC, B_DC };
        u8 left_nz[9] = { 0 };
        bd_t *tp = &parts[my & (nparts - 1)];
        for(int mx = 0; mx < v->mbw; mx++){
            int seg = 0;
            if(v->seg_map){ int b = bd_get(&bd, v->seg_prob[0]); seg = b ? 2 + bd_get(&bd, v->seg_prob[2]) : bd_get(&bd, v->seg_prob[1]); }
            int skip = v->skip_on ? bd_get(&bd, v->skip_prob) : 0;
            int ym = bd_tree(&bd, ymode_tree, vp8_ymode_prob);
            u8 bm[16];
            if(ym == M_SPLIT){
                for(int y = 0; y < 4; y++) for(int x = 0; x < 4; x++){
                    int m = bd_tree(&bd, bmode_tree, vp8_bmode_prob[top_b[mx * 4 + x]][left_b[y]]);
                    bm[y * 4 + x] = (u8)m;
                    top_b[mx * 4 + x] = left_b[y] = (u8)m;
                }
            } else {
                memset(top_b + mx * 4, ymode_to_b[ym], 4);
                memset(left_b, ymode_to_b[ym], 4);
            }
            int uvm = bd_tree(&bd, uvmode_tree, vp8_uvmode_prob);

            /* the coefficients */
            memset(blk, 0, sizeof blk);
            u8 *tnz = top_nz + mx * 9;
            int any = 0;
            u8 nzmask[25];
            memset(nzmask, 0, sizeof nzmask);
            if(!skip){
                int first = 0, ytype = 3;
                if(ym != M_SPLIT){
                    int ctx = tnz[8] + left_nz[8];
                    int n = read_coeffs(tp, v, 1, ctx, 0, v->dq[seg][1], blk[24]);
                    tnz[8] = left_nz[8] = n > 0;
                    nzmask[24] = n > 0;
                    any |= n > 0;
                    first = 1; ytype = 0;
                }
                for(int y = 0; y < 4; y++) for(int x = 0; x < 4; x++){
                    int ctx = tnz[x] + left_nz[y];
                    int n = read_coeffs(tp, v, ytype, ctx, first, v->dq[seg][0], blk[y * 4 + x]);
                    tnz[x] = left_nz[y] = n > 0;
                    nzmask[y * 4 + x] = n > 0;
                    any |= n > 0;
                }
                for(int c = 0; c < 2; c++) for(int y = 0; y < 2; y++) for(int x = 0; x < 2; x++){
                    int ctx = tnz[4 + c * 2 + x] + left_nz[4 + c * 2 + y];
                    int n = read_coeffs(tp, v, 2, ctx, 0, v->dq[seg][2], blk[16 + c * 4 + y * 2 + x]);
                    tnz[4 + c * 2 + x] = left_nz[4 + c * 2 + y] = n > 0;
                    nzmask[16 + c * 4 + y * 2 + x] = n > 0;
                    any |= n > 0;
                }
            } else {
                memset(tnz, 0, 8); memset(left_nz, 0, 8);
                if(ym != M_SPLIT){ tnz[8] = 0; left_nz[8] = 0; }
            }
            if(ym != M_SPLIT && nzmask[24]){
                short dc[16][16];
                iwht(blk[24], dc);
                for(int i = 0; i < 16; i++){ blk[i][0] = dc[i][0]; if(dc[i][0]) nzmask[i] = 1; }
            }

            /* prediction and reconstruction */
            u8 *Y = v->y + my * 16 * v->ys + mx * 16;
            if(ym != M_SPLIT){
                pred_block(Y, v->ys, 16, ym, my > 0, mx > 0);
                for(int i = 0; i < 16; i++) if(nzmask[i]) idct_add(blk[i], Y + (i / 4) * 4 * v->ys + (i % 4) * 4, v->ys);
            } else {
                /* the pixels above and to the right of the macroblock, for the 4x4 blocks on its right */
                u8 ar[4];
                const u8 *above = Y - v->ys;
                if(my == 0) memset(ar, 127, 4);
                else if(mx == v->mbw - 1) memset(ar, above[15], 4);
                else memcpy(ar, above + 16, 4);
                for(int y = 0; y < 4; y++) for(int x = 0; x < 4; x++){
                    u8 *dst = Y + y * 4 * v->ys + x * 4;
                    u8 Abuf[9];
                    const u8 *src = dst - v->ys;
                    Abuf[0] = src[-1];
                    memcpy(Abuf + 1, src, 4);
                    if(x < 3) memcpy(Abuf + 5, src + 4, 4);
                    else memcpy(Abuf + 5, ar, 4);
                    u8 L[4];
                    for(int k = 0; k < 4; k++) L[k] = dst[k * v->ys - 1];
                    pred4(dst, v->ys, bm[y * 4 + x], Abuf + 1, L);
                    if(nzmask[y * 4 + x]) idct_add(blk[y * 4 + x], dst, v->ys);
                }
            }
            u8 *U = v->u + my * 8 * v->uvs + mx * 8, *V = v->v + my * 8 * v->uvs + mx * 8;
            pred_block(U, v->uvs, 8, uvm, my > 0, mx > 0);
            pred_block(V, v->uvs, 8, uvm, my > 0, mx > 0);
            for(int i = 0; i < 4; i++){
                if(nzmask[16 + i]) idct_add(blk[16 + i], U + (i / 2) * 4 * v->uvs + (i % 2) * 4, v->uvs);
                if(nzmask[20 + i]) idct_add(blk[20 + i], V + (i / 2) * 4 * v->uvs + (i % 2) * 4, v->uvs);
            }

            /* this macroblock's loop filter strength */
            int lvl = v->flevel;
            if(v->seg_on) lvl = v->seg_abs ? v->seg_lf[seg] : lvl + v->seg_lf[seg];
            lvl = lvl < 0 ? 0 : lvl > 63 ? 63 : lvl;
            if(v->lf_adj){
                lvl += v->ref_delta[0];
                if(ym == M_SPLIT) lvl += v->mode_delta[0];
                lvl = lvl < 0 ? 0 : lvl > 63 ? 63 : lvl;
            }
            v->lf_level[my * v->mbw + mx] = (u8)lvl;
            v->lf_inner[my * v->mbw + mx] = (u8)(any || ym == M_SPLIT);
        }
    }
    kfree(top_b); kfree(top_nz);
    if(v->flevel) loop_filter(v);
    return 0;
}

/* YUV 4:2:0 to RGB (BT.601, as libwebp), chroma upsampled with 9:3:3:1 weights */
static void vp8_to_rgb(const vp8_t *v, u32 *out, int W, int H){
    int cw = (W + 1) / 2, chh = (H + 1) / 2;
    for(int y = 0; y < H; y++){
        int cy0 = y >> 1, cy1 = (y & 1) ? cy0 + 1 : cy0 - 1;
        if(cy1 < 0) cy1 = 0;
        if(cy1 >= chh) cy1 = chh - 1;
        const u8 *Yr = v->y + y * v->ys;
        const u8 *U0 = v->u + cy0 * v->uvs, *U1 = v->u + cy1 * v->uvs;
        const u8 *V0 = v->v + cy0 * v->uvs, *V1 = v->v + cy1 * v->uvs;
        for(int x = 0; x < W; x++){
            int cx0 = x >> 1, cx1 = (x & 1) ? cx0 + 1 : cx0 - 1;
            if(cx1 < 0) cx1 = 0;
            if(cx1 >= cw) cx1 = cw - 1;
            int u = (9 * U0[cx0] + 3 * U0[cx1] + 3 * U1[cx0] + U1[cx1] + 8) >> 4;
            int vv = (9 * V0[cx0] + 3 * V0[cx1] + 3 * V1[cx0] + V1[cx1] + 8) >> 4;
            int yy = Yr[x];
            /* libwebp's 14-bit fixed point (VP8YUVToR/G/B) */
            int R = ((19077 * yy >> 8) + (26149 * vv >> 8) - 14234) >> 6;
            int G = ((19077 * yy >> 8) - (6419 * u >> 8) - (13320 * vv >> 8) + 8708) >> 6;
            int B = ((19077 * yy >> 8) + (33050 * u >> 8) - 17685) >> 6;
            out[y * W + x] = (u32)clip8(R) << 16 | (u32)clip8(G) << 8 | clip8(B);
        }
    }
}

/* ======================= the container ======================= */

/* the alpha of a lossy picture (an ALPH chunk) into the top bytes of px */
static void apply_alpha(const u8 *a, u32 n, u32 *px, int W, int H){
    if(n < 1) return;
    int method = a[0] & 3, filt = (a[0] >> 2) & 3;
    u8 *al = kmalloc((u32)W * (u32)H);
    if(!al) return;
    int ok = 0;
    if(method == 0){ if(n - 1 >= (u32)W * (u32)H){ memcpy(al, a + 1, (u32)W * (u32)H); ok = 1; } }
    else if(method == 1){
        lbr_t b = { a + 1, a + n, 0, 0, 0 };
        u32 *g = vp8l_decode_stream(&b, W, H);
        if(g){ for(int i = 0; i < W * H; i++) al[i] = (u8)(g[i] >> 8); kfree(g); ok = 1; }
    }
    if(ok){
        for(int y = 0; y < H; y++) for(int x = 0; x < W; x++){
            int i = y * W + x, pred = 0;
            if(filt == 0) break;
            if(x == 0 && y == 0) pred = 0;
            else if(y == 0) pred = al[i - 1];
            else if(x == 0) pred = al[i - W];
            else if(filt == 1) pred = al[i - 1];
            else if(filt == 2) pred = al[i - W];
            else pred = clip8(al[i - 1] + al[i - W] - al[i - W - 1]);
            al[i] = (u8)(al[i] + pred);
        }
        for(int i = 0; i < W * H; i++) px[i] = (px[i] & 0xFFFFFFu) | (u32)(255 - al[i]) << 24;
    }
    kfree(al);
}

static int decode_frame(const u8 *vp8, u32 vlen, const u8 *alph, u32 alen, int lossless, u32 **out, u32 *w, u32 *h){
    if(lossless){
        if(vlen < 5 || vp8[0] != 0x2F) return -1;
        u32 hdr = rd32(vp8 + 1);
        int W = (int)(hdr & 0x3FFF) + 1, H = (int)((hdr >> 14) & 0x3FFF) + 1;
        if((hdr >> 29) != 0) return -1;                      /* version */
        if((u64)W * H > WEBP_MAX_PIX) return -1;
        lbr_t b = { vp8 + 5, vp8 + vlen, 0, 0, 0 };
        u32 *px = vp8l_decode_stream(&b, W, H);
        if(!px) return -1;
        for(int i = 0; i < W * H; i++){ u32 p = px[i]; px[i] = (p & 0xFFFFFFu) | (u32)(255 - (p >> 24)) << 24; }
        *out = px; *w = (u32)W; *h = (u32)H;
        return 0;
    }
    vp8_t v;
    if(vp8_decode(&v, vp8, vlen) != 0){ vp8_free(&v); return -1; }
    u32 *px = kmalloc(4u * (u32)v.W * (u32)v.H);
    if(!px){ vp8_free(&v); return -1; }
    vp8_to_rgb(&v, px, v.W, v.H);
    if(alph) apply_alpha(alph, alen, px, v.W, v.H);
    *out = px; *w = (u32)v.W; *h = (u32)v.H;
    vp8_free(&v);
    return 0;
}

/* Finds the picture chunks among the chunks at p..end. */
static int find_chunks(const u8 *p, const u8 *end, const u8 **img, u32 *ilen, int *lossless, const u8 **alph, u32 *alen,
                       const u8 **anmf, u32 *anlen){
    while(p + 8 <= end){
        u32 n = rd32(p + 4);
        const u8 *d = p + 8;
        if(d + n > end) n = (u32)(end - d);
        if(!memcmp(p, "VP8 ", 4) && !*img){ *img = d; *ilen = n; *lossless = 0; }
        else if(!memcmp(p, "VP8L", 4) && !*img){ *img = d; *ilen = n; *lossless = 1; }
        else if(!memcmp(p, "ALPH", 4) && !*alph){ *alph = d; *alen = n; }
        else if(!memcmp(p, "ANMF", 4) && anmf && !*anmf){ *anmf = d; *anlen = n; }
        p = d + n + (n & 1);
    }
    return *img ? 0 : -1;
}

int webp_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h){
    if(len < 20 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WEBP", 4)) return -1;
    u32 riff = rd32(data + 4);
    const u8 *end = data + (riff + 8 < len ? riff + 8 : len);
    const u8 *img = NULL, *alph = NULL, *anmf = NULL;
    u32 ilen = 0, alen = 0, anlen = 0;
    int lossless = 0;
    find_chunks(data + 12, end, &img, &ilen, &lossless, &alph, &alen, &anmf, &anlen);
    if(!img && anmf && anlen > 16){
        /* an animation: its first frame, placed on the canvas */
        const u8 *vp8x = NULL;
        for(const u8 *p = data + 12; p + 8 <= end; ){ u32 n = rd32(p + 4); if(!memcmp(p, "VP8X", 4)) vp8x = p + 8; p += 8 + n + (n & 1); }
        if(find_chunks(anmf + 16, anmf + anlen, &img, &ilen, &lossless, &alph, &alen, NULL, NULL) != 0) return -1;
        u32 *fp, fw, fh;
        if(decode_frame(img, ilen, alph, alen, lossless, &fp, &fw, &fh) != 0) return -1;
        u32 cw = vp8x ? rd24(vp8x + 4) + 1 : fw, ch = vp8x ? rd24(vp8x + 7) + 1 : fh;
        u32 fx = rd24(anmf) * 2, fy = rd24(anmf + 3) * 2;
        if(cw == fw && ch == fh && !fx && !fy){ *out_px = fp; *out_w = fw; *out_h = fh; return 0; }
        if((u64)cw * ch > WEBP_MAX_PIX){ kfree(fp); return -1; }
        u32 *c = kmalloc(4 * cw * ch);
        if(!c){ kfree(fp); return -1; }
        for(u32 i = 0; i < cw * ch; i++) c[i] = 0xFF000000u;          /* transparent */
        for(u32 y = 0; y < fh && fy + y < ch; y++) for(u32 x = 0; x < fw && fx + x < cw; x++) c[(fy + y) * cw + fx + x] = fp[y * fw + x];
        kfree(fp);
        *out_px = c; *out_w = cw; *out_h = ch;
        return 0;
    }
    if(!img) return -1;
    return decode_frame(img, ilen, alph, alen, lossless, out_px, out_w, out_h);
}
