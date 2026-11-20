/* yuv.c — decoded pictures (planar YUV) to XRGB8888 pixels, scaled to any size.
 *
 * The conversion runs 8 pixels at a time in SSE2 (16-bit fixed point), because under
 * QEMU's TCG the vector instructions are translated to host vector code and a scalar
 * per-pixel loop costs more than decoding the frame. A different output size is then made
 * from the converted picture: nearest-neighbour (rows that repeat are copied whole), or
 * bilinear when the caller asks for it (a paused picture).
 * BT.601 and BT.709, limited or full range; 4:2:0, 4:2:2 and 4:4:4. */
#include "media.h"
#include <stdlib.h>
#include <string.h>
#include <emmintrin.h>

typedef struct { int16_t yoff, cy, crv, cgu, cgv, cbu; } coef_t;   /* Q14, cbu Q13 */

static coef_t coefs(int bt709, int full){
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    double ys = full ? 1.0 : 255.0 / 219.0, cs = full ? 1.0 : 255.0 / 224.0;
    coef_t c;
    c.yoff = full ? 0 : 16;
    c.cy  = (int16_t)(ys * 16384 + 0.5);
    c.crv = (int16_t)(cs * 2.0 * (1.0 - kr) * 16384 + 0.5);
    c.cgu = (int16_t)(cs * 2.0 * (1.0 - kb) * kb / kg * 16384 + 0.5);
    c.cgv = (int16_t)(cs * 2.0 * (1.0 - kr) * kr / kg * 16384 + 0.5);
    c.cbu = (int16_t)(cs * 2.0 * (1.0 - kb) * 8192 + 0.5);
    return c;
}

static inline uint8_t clamp8(int v){ return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* one pixel, the same maths as the vector code */
static inline uint32_t px1(const coef_t *c, int y, int u, int v){
    int yy = ((y - c->yoff) * 64 * c->cy) >> 16;
    int uu = (u - 128) * 64, vv = (v - 128) * 64;
    int r = yy + ((vv * c->crv) >> 16);
    int g = yy - ((uu * c->cgu) >> 16) - ((vv * c->cgv) >> 16);
    int b = yy + (((uu * c->cbu) >> 16) << 1);
    return (uint32_t)clamp8((r + 8) >> 4) << 16 | (uint32_t)clamp8((g + 8) >> 4) << 8 | clamp8((b + 8) >> 4);
}

/* one row: w pixels from Y and chroma rows (chroma halved horizontally when half) */
static void row(const coef_t *c, const uint8_t *yr, const uint8_t *ur, const uint8_t *vr, int half, uint32_t *o, int w){
    const __m128i zero = _mm_setzero_si128();
    const __m128i yoff = _mm_set1_epi16(c->yoff), c128 = _mm_set1_epi16(128);
    const __m128i cy = _mm_set1_epi16(c->cy), crv = _mm_set1_epi16(c->crv), cgu = _mm_set1_epi16(c->cgu);
    const __m128i cgv = _mm_set1_epi16(c->cgv), cbu = _mm_set1_epi16(c->cbu), rnd = _mm_set1_epi16(8);
    const __m128i alpha = _mm_set1_epi8((char)0xff);
    int x = 0;
    for(; x + 8 <= w; x += 8){
        __m128i y8 = _mm_loadl_epi64((const __m128i *)(yr + x));
        __m128i u8, v8;
        if(half){
            u8 = _mm_cvtsi32_si128(*(const int32_t *)(ur + x / 2));
            v8 = _mm_cvtsi32_si128(*(const int32_t *)(vr + x / 2));
            u8 = _mm_unpacklo_epi8(u8, u8);
            v8 = _mm_unpacklo_epi8(v8, v8);
        } else {
            u8 = _mm_loadl_epi64((const __m128i *)(ur + x));
            v8 = _mm_loadl_epi64((const __m128i *)(vr + x));
        }
        __m128i y16 = _mm_slli_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(y8, zero), yoff), 6);
        __m128i u16 = _mm_slli_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(u8, zero), c128), 6);
        __m128i v16 = _mm_slli_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(v8, zero), c128), 6);
        __m128i yy = _mm_mulhi_epi16(y16, cy);
        __m128i r = _mm_add_epi16(yy, _mm_mulhi_epi16(v16, crv));
        __m128i g = _mm_sub_epi16(_mm_sub_epi16(yy, _mm_mulhi_epi16(u16, cgu)), _mm_mulhi_epi16(v16, cgv));
        __m128i b = _mm_add_epi16(yy, _mm_slli_epi16(_mm_mulhi_epi16(u16, cbu), 1));
        r = _mm_srai_epi16(_mm_add_epi16(r, rnd), 4);
        g = _mm_srai_epi16(_mm_add_epi16(g, rnd), 4);
        b = _mm_srai_epi16(_mm_add_epi16(b, rnd), 4);
        __m128i r8 = _mm_packus_epi16(r, r), g8 = _mm_packus_epi16(g, g), b8 = _mm_packus_epi16(b, b);
        __m128i bg = _mm_unpacklo_epi8(b8, g8);         /* B G B G ... */
        __m128i ra = _mm_unpacklo_epi8(r8, alpha);      /* R X R X ... */
        _mm_storeu_si128((__m128i *)(o + x), _mm_unpacklo_epi16(bg, ra));
        _mm_storeu_si128((__m128i *)(o + x + 4), _mm_unpackhi_epi16(bg, ra));
    }
    for(; x < w; x++){
        int cx = half ? x >> 1 : x;
        o[x] = px1(c, yr[x], ur[cx], vr[cx]) | 0xff000000u;
    }
}

static void convert(const mpic_t *p, uint32_t *dst, int dstride){
    coef_t c = coefs(p->bt709 ? 1 : 0, p->full_range ? 1 : 0);
    int half = p->cw < p->w, vhalf = p->ch < p->h;
    for(int y = 0; y < p->h; y++){
        int cy = vhalf ? y >> 1 : y;
        row(&c, p->y + (size_t)y * p->ys, p->u + (size_t)cy * p->uvs, p->v + (size_t)cy * p->uvs, half,
            dst + (size_t)y * dstride, p->w);
    }
}

/* the converted picture at its own size, when the output is another size */
static uint32_t *native;
static size_t native_cap;
static int *xt, xt_w, xt_dw;

static void scale_nearest(const uint32_t *src, int w, int h, uint32_t *dst, int dw, int dh, int dstride){
    if(xt_w != w || xt_dw != dw){
        free(xt);
        xt = malloc(sizeof(int) * (size_t)dw);
        if(!xt){ xt_w = 0; return; }
        for(int x = 0; x < dw; x++){ int sx = (int)(((int64_t)(2 * x + 1) * w) / (2 * dw)); xt[x] = sx < w ? sx : w - 1; }
        xt_w = w; xt_dw = dw;
    }
    int last = -1;
    for(int y = 0; y < dh; y++){
        int sy = (int)(((int64_t)(2 * y + 1) * h) / (2 * dh));
        if(sy >= h) sy = h - 1;
        uint32_t *o = dst + (size_t)y * dstride;
        if(sy == last){ memcpy(o, o - dstride, (size_t)dw * 4); continue; }
        const uint32_t *s = src + (size_t)sy * w;
        for(int x = 0; x < dw; x++) o[x] = s[xt[x]];
        last = sy;
    }
}

static void scale_bilinear(const uint32_t *src, int w, int h, uint32_t *dst, int dw, int dh, int dstride){
    for(int y = 0; y < dh; y++){
        int64_t sy = ((int64_t)(2 * y + 1) * h * 128) / dh - 128;
        if(sy < 0) sy = 0;
        int iy = (int)(sy >> 8), wy = (int)(sy & 255);
        if(iy >= h - 1){ iy = h - 1; wy = 0; }
        const uint32_t *r0 = src + (size_t)iy * w, *r1 = iy + 1 < h ? r0 + w : r0;
        uint32_t *o = dst + (size_t)y * dstride;
        for(int x = 0; x < dw; x++){
            int64_t sx = ((int64_t)(2 * x + 1) * w * 128) / dw - 128;
            if(sx < 0) sx = 0;
            int ix = (int)(sx >> 8), wx = (int)(sx & 255);
            if(ix >= w - 1){ ix = w - 1; wx = 0; }
            int ix1 = ix + 1 < w ? ix + 1 : ix;
            uint32_t a = r0[ix], b = r0[ix1], c = r1[ix], d = r1[ix1], out = 0xff000000u;
            for(int sh = 0; sh < 24; sh += 8){
                int top = (int)(a >> sh & 255) * (256 - wx) + (int)(b >> sh & 255) * wx;
                int bot = (int)(c >> sh & 255) * (256 - wx) + (int)(d >> sh & 255) * wx;
                out |= (uint32_t)((top * (256 - wy) + bot * wy + 32768) >> 16) << sh;
            }
            o[x] = out;
        }
    }
}

void yuv_to_rgb_q(const mpic_t *p, uint32_t *dst, int dw, int dh, int dstride, int smooth){
    if(dw <= 0 || dh <= 0 || p->w <= 0 || p->h <= 0) return;
    if(dw == p->w && dh == p->h){ convert(p, dst, dstride); return; }
    size_t need = (size_t)p->w * p->h;
    if(need > native_cap){
        free(native);
        native = malloc(need * 4);
        native_cap = native ? need : 0;
        if(!native) return;
    }
    convert(p, native, p->w);
    if(smooth) scale_bilinear(native, p->w, p->h, dst, dw, dh, dstride);
    else scale_nearest(native, p->w, p->h, dst, dw, dh, dstride);
}

void yuv_to_rgb(const mpic_t *p, uint32_t *dst, int dw, int dh, int dstride){
    yuv_to_rgb_q(p, dst, dw, dh, dstride, 0);
}
