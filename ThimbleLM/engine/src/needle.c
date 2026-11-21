/*
 * Needle 3 (Cactus Compute, Apache-2.0) from its shipped needle3.cact.
 *
 * The architecture is cactus/model/architecture.py: a 20-layer decoder whose residual stream
 * is four lanes mixed by learned maps (mHC: a Sinkhorn-normalised 4x4 per layer), attention
 * with 12 query / 2 KV heads, a 3-tap causal conv on Q/K/V, RMS-normed Q/K, RoPE, a sigmoid
 * output gate, local (1024) and global layers; an MLP made of three 32x32 Kronecker factors
 * with diagonals and two fixed permutations; and at five layers an "engram": hashed bigram
 * and trigram tables whose looked-up rows are gated into the stream. A probe head over every
 * layer's hidden state gives a calibrated confidence. ThimbleLM/needle/needle_ref.py is the
 * numpy translation this file is checked against, line for line.
 *
 * Numerics are the archive's: matrices are CQ (Hadamard-rotated groups of 128 coded with a
 * 4- or 16-level codebook and an FP16 norm), activations are int8 per row (round half to
 * even), the KV cache holds int8-rounded values. At load the CQ matrices are rebuilt once
 * (a fast Walsh-Hadamard transform per group) into int8 blocks of 32, so every product runs
 * on the same integer kernels as the speech engine; the engram tables (two thirds of the
 * file) and the embedding rows stay packed and are rebuilt per looked-up row.
 */
#include "../include/thimble_needle.h"
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#endif

#define BLOCK   32
#define TAG     0x05E12A84u
#define MAXT    1024            /* positions: tools + query + answer */
#define CH      32              /* positions per batched pass */
#define MAXSITE 8
#define MAXL    24

/* ---------------- allocator and clock ---------------- */
static void *(*g_alloc)(size_t) = malloc;
static void (*g_free)(void *) = free;
static double (*g_clock)(void) = NULL;
void thim_needle_set_alloc(void *(*a)(size_t), void (*r)(void *)){ g_alloc = a ? a : malloc; g_free = r ? r : free; }
void thim_needle_set_clock(double (*c)(void)){ g_clock = c; }
static double now(void){ return g_clock ? g_clock() : 0.0; }
static void *xalloc(size_t n){ void *p = g_alloc(n + 64); if(!p) return NULL; memset(p, 0, n + 64); return p; }
static void xfree(void *p){ if(p) g_free(p); }

static void copy_err(char *err, size_t cap, const char *msg){
    if(!err || !cap) return;
    size_t i = 0;
    for(; msg[i] && i + 1 < cap; i++) err[i] = msg[i];
    err[i] = 0;
}

/* ---------------- small math (no libm) ---------------- */
static inline float fsqrt(float x){ return __builtin_sqrtf(x); }
static float fexp(float x){
    if(x < -87.f) return 0.f;
    if(x > 88.f) x = 88.f;
    float t = x * 1.44269504089f;
    int n = (int)(t < 0 ? t - 1 : t);
    float f = (t - (float)n) * 0.69314718056f;
    float p = 1.f + f * (1.f + f * (0.5f + f * (1.f / 6 + f * (1.f / 24 + f * (1.f / 120 + f * (1.f / 720))))));
    union { float f; uint32_t u; } v; v.u = (uint32_t)(n + 127) << 23;
    return p * v.f;
}
static float flog(float x){                             /* x > 0 */
    union { float f; uint32_t u; } v; v.f = x;
    int e = (int)((v.u >> 23) & 255) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f, s = (m - 1.f) / (m + 1.f), s2 = s * s;
    float l = s * (2.88539008f + s2 * (0.96179669f + s2 * (0.57707801f + s2 * (0.41219858f + s2 * 0.32059890f))));
    return ((float)e + l) * 0.69314718056f;
}
static inline float sigm(float x){ return 1.f / (1.f + fexp(-x)); }
/* round half to even, like jnp.round (|x| < 2^22 here) */
static inline float rne(float x){
    volatile float m = 12582912.f;                      /* 1.5 * 2^23 */
    return x >= 0 ? (x + m) - m : (x - m) + m;
}
static float f16(uint16_t h){
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if(e == 0){
        if(!m) u = s;
        else { e = 127 - 15 + 1; while(!(m & 1024)){ m <<= 1; e--; } m &= 1023; u = s | (e << 23) | (m << 13); }
    } else if(e == 31) u = s | 0x7F800000u | (m << 13);
    else u = s | ((e + 127 - 15) << 23) | (m << 13);
    union { uint32_t u; float f; } v; v.u = u;
    return v.f;
}
/* sin and cos of a (|a| < 1e5) with the reduction done in double */
static void fsincos(double a, float *s, float *c){
    const double PI2 = 6.283185307179586476925, PIO2 = 1.570796326794896619231;
    double k = a / PI2; long long ki = (long long)(k < 0 ? k - 0.5 : k + 0.5);
    double r = a - (double)ki * PI2;                    /* [-pi, pi] */
    long long q = (long long)(r / PIO2 + (r < 0 ? -0.5 : 0.5));
    double x = r - (double)q * PIO2, x2 = x * x;        /* [-pi/4, pi/4] */
    double sn = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
    double cs = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
    switch((int)(q & 3)){
    case 0: *s = (float)sn; *c = (float)cs; break;
    case 1: *s = (float)cs; *c = (float)-sn; break;
    case 2: *s = (float)-sn; *c = (float)-cs; break;
    default: *s = (float)-cs; *c = (float)sn; break;
    }
}

/* ---------------- int8 matrices and kernels ---------------- */
typedef struct { int rows, cols, cp; int8_t *q; float *sc; } mat_t;

#if defined(__AVX2__) || defined(__SSSE3__)
static inline float hsum_ps128(__m128 v){ v = _mm_add_ps(v, _mm_movehl_ps(v, v)); v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1)); return _mm_cvtss_f32(v); }
#endif

/* four rows of activations against one row of weights */
static void tile4(const int8_t *wr, const float *ws, const int8_t *const a[4], const float *const as[4], int nb, float out[4]){
#if defined(__AVX2__)
    __m256 acc0 = _mm256_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
    const __m256i one = _mm256_set1_epi16(1);
    for(int b = 0; b < nb; b++){
        __m256i x = _mm256_loadu_si256((const __m256i *)(wr + b * BLOCK));
        __m256i ax = _mm256_sign_epi8(x, x);
        float w = ws[b];
        #define STEP(i, acc) { __m256i y = _mm256_loadu_si256((const __m256i *)(a[i] + b * BLOCK)); \
            __m256i t = _mm256_madd_epi16(_mm256_maddubs_epi16(ax, _mm256_sign_epi8(y, x)), one); \
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(t), _mm256_set1_ps(w * as[i][b]), acc); }
        STEP(0, acc0) STEP(1, acc1) STEP(2, acc2) STEP(3, acc3)
        #undef STEP
    }
    #define RED(acc) hsum_ps128(_mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1)))
    out[0] = RED(acc0); out[1] = RED(acc1); out[2] = RED(acc2); out[3] = RED(acc3);
    #undef RED
#elif defined(__SSSE3__)
    __m128 acc0 = _mm_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
    const __m128i one = _mm_set1_epi16(1);
    for(int b = 0; b < nb; b++){
        __m128i x0 = _mm_loadu_si128((const __m128i *)(wr + b * BLOCK)), x1 = _mm_loadu_si128((const __m128i *)(wr + b * BLOCK + 16));
        __m128i ax0 = _mm_sign_epi8(x0, x0), ax1 = _mm_sign_epi8(x1, x1);
        float w = ws[b];
        #define STEP(i, acc) { __m128i y0 = _mm_loadu_si128((const __m128i *)(a[i] + b * BLOCK)), y1 = _mm_loadu_si128((const __m128i *)(a[i] + b * BLOCK + 16)); \
            __m128i t = _mm_add_epi32(_mm_madd_epi16(_mm_maddubs_epi16(ax0, _mm_sign_epi8(y0, x0)), one), _mm_madd_epi16(_mm_maddubs_epi16(ax1, _mm_sign_epi8(y1, x1)), one)); \
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_cvtepi32_ps(t), _mm_set1_ps(w * as[i][b]))); }
        STEP(0, acc0) STEP(1, acc1) STEP(2, acc2) STEP(3, acc3)
        #undef STEP
    }
    out[0] = hsum_ps128(acc0); out[1] = hsum_ps128(acc1); out[2] = hsum_ps128(acc2); out[3] = hsum_ps128(acc3);
#else
    for(int i = 0; i < 4; i++){
        float acc = 0.f;
        for(int b = 0; b < nb; b++){ int t = 0; for(int j = 0; j < BLOCK; j++) t += (int)wr[b * BLOCK + j] * (int)a[i][b * BLOCK + j]; acc += (float)t * ws[b] * as[i][b]; }
        out[i] = acc;
    }
#endif
}

/* one row of activations against four rows of weights (a token at a time) */
static void rows4(const int8_t *const w[4], const float *const ws[4], const int8_t *a, const float *as, int nb, float out[4]){
#if defined(__AVX2__)
    __m256 acc0 = _mm256_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
    const __m256i one = _mm256_set1_epi16(1);
    for(int b = 0; b < nb; b++){
        __m256i y = _mm256_loadu_si256((const __m256i *)(a + b * BLOCK));
        float sb = as[b];
        #define STEP(i, acc) { __m256i x = _mm256_loadu_si256((const __m256i *)(w[i] + b * BLOCK)); \
            __m256i t = _mm256_madd_epi16(_mm256_maddubs_epi16(_mm256_sign_epi8(x, x), _mm256_sign_epi8(y, x)), one); \
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(t), _mm256_set1_ps(ws[i][b] * sb), acc); }
        STEP(0, acc0) STEP(1, acc1) STEP(2, acc2) STEP(3, acc3)
        #undef STEP
    }
    #define RED(acc) hsum_ps128(_mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1)))
    out[0] = RED(acc0); out[1] = RED(acc1); out[2] = RED(acc2); out[3] = RED(acc3);
    #undef RED
#elif defined(__SSSE3__)
    __m128 acc0 = _mm_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
    const __m128i one = _mm_set1_epi16(1);
    for(int b = 0; b < nb; b++){
        __m128i y0 = _mm_loadu_si128((const __m128i *)(a + b * BLOCK)), y1 = _mm_loadu_si128((const __m128i *)(a + b * BLOCK + 16));
        float sb = as[b];
        #define STEP(i, acc) { __m128i x0 = _mm_loadu_si128((const __m128i *)(w[i] + b * BLOCK)), x1 = _mm_loadu_si128((const __m128i *)(w[i] + b * BLOCK + 16)); \
            __m128i t = _mm_add_epi32(_mm_madd_epi16(_mm_maddubs_epi16(_mm_sign_epi8(x0, x0), _mm_sign_epi8(y0, x0)), one), _mm_madd_epi16(_mm_maddubs_epi16(_mm_sign_epi8(x1, x1), _mm_sign_epi8(y1, x1)), one)); \
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_cvtepi32_ps(t), _mm_set1_ps(ws[i][b] * sb))); }
        STEP(0, acc0) STEP(1, acc1) STEP(2, acc2) STEP(3, acc3)
        #undef STEP
    }
    out[0] = hsum_ps128(acc0); out[1] = hsum_ps128(acc1); out[2] = hsum_ps128(acc2); out[3] = hsum_ps128(acc3);
#else
    for(int i = 0; i < 4; i++){
        float acc = 0.f;
        for(int b = 0; b < nb; b++){ int t = 0; for(int j = 0; j < BLOCK; j++) t += (int)w[i][b * BLOCK + j] * (int)a[b * BLOCK + j]; acc += (float)t * ws[i][b] * as[b]; }
        out[i] = acc;
    }
#endif
}

/* ---------------- the model ---------------- */
typedef struct {
    float *norm_in, *q_taps, *k_taps, *v_taps, *q_norm, *k_norm, *post_norm, *pre_hada;
    float attn_gate;
    float *d1, *d2, *b2, *d3, *d4, *w[6], *cond_v, *cond_u;
    mat_t qkvg, out, phi;                 /* phi: 4 pre, 4 post, 16 res rows */
} layer_t;

typedef struct {
    int layer;
    const uint8_t *tab_packed; const uint16_t *tab_norms; int tab_bits;
    mat_t kv;                             /* key rows then value rows */
    float *taps;                          /* [4][d] */
} site_t;

/* the histories a position needs from before a batch: conv taps and engram taps */
typedef struct {
    int len;
    float *raw_hist;                      /* [L][2][qkv] raw projections of t-1, t-2 */
    float *ev_hist;                       /* [sites][9][d] engram value rows of t-1 .. t-9 */
    float *cm, *cs, *cacc;                /* confidence: running max, sum, weighted sum per (row, probe) */
} snap_t;

struct thim_needle {
    const uint8_t *base; size_t len;
    int vocab, out_vocab, d, nh, nkv, L, qk, vd, hada_n, lanes, taps, window, eg_slots, eg_sub, eg_tables;
    int eg_ctaps, eg_dil, n_orders, orders[4], n_sites, kv_window;
    uint32_t gmask_lo, gmask_hi;
    float rope_theta, cb[3][16];
    int qkv;                              /* q + k + v widths */
    /* weights */
    const uint8_t *emb_packed; const uint16_t *emb_norms; int emb_bits;
    mat_t emb;                            /* int8 copy for the output head */
    layer_t ly[MAXL];
    float a_pre[MAXL], a_post[MAXL], a_res[MAXL], *b_pre, *b_post, *b_res;
    int *p1, *p2;
    site_t site[MAXSITE];
    int site_of_layer[MAXL];
    float *final_norm;
    int has_head; float *probes, *gain, *query, *row_bias, *proj, proj_bias; int n_probe, n_query;
    /* tokenizer */
    int n_pieces; const char **piece; uint16_t *piece_len; float *score; uint8_t *ptype;
    int *hash; int hash_cap;
    int bos, eos, unk, byte_id[256];
    int markers[32], n_markers;
    int tk_think, tk_think_end, tk_call, tk_call_end, tk_im_end, tk_im_start;
    /* state */
    int *tokens; int n;
    float *K, *V;                         /* [L][MAXT][nkv*qk], [L][MAXT][nkv*vd] */
    snap_t cur, pre;
    float *cosb, *sinb;                   /* [MAXT][qk/2] */
    int8_t *qa; float *qs; size_t qa_cap;
    float *X, *x0, *u, *hb, *nx, *mo, *raw, *qb, *kb, *vb, *att, *e, *ekv, *ek, *ev, *sc, *z, *zt, *cond, *logits, *tmp;
    /* tools */
    char *tools_json, *system, *tools_lc; int prefix_ready;
    struct gram *g;
};

static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p){ uint64_t v; memcpy(&v, p, 8); return v; }

typedef struct { int dtype, ndim, shape[4]; uint64_t off, nbytes; int group, bits; } rec_t;
#define HDR_BYTES 196
#define REC_BYTES 44

static int read_rec(const thim_needle *n, int cb_n, int i, rec_t *r){
    const uint8_t *p = n->base + HDR_BYTES + (size_t)cb_n * 4 + (size_t)i * REC_BYTES;
    if((size_t)(p + REC_BYTES - n->base) > n->len) return 0;
    r->dtype = p[0]; r->ndim = p[1];
    for(int k = 0; k < 4; k++) r->shape[k] = (int)rd32(p + 4 + 4 * k);
    r->off = rd64(p + 20); r->nbytes = rd64(p + 28);
    r->group = (int)rd32(p + 36); r->bits = (int)rd32(p + 40);
    return r->off + r->nbytes <= n->len;
}

/* normalised Walsh-Hadamard transform in place (n a power of two) */
static void fwht(float *v, int n){
    for(int h = 1; h < n; h <<= 1)
        for(int i = 0; i < n; i += h << 1)
            for(int j = i; j < i + h; j++){ float a = v[j], b = v[j + h]; v[j] = a + b; v[j + h] = a - b; }
    float s = 1.f / fsqrt((float)n);
    for(int i = 0; i < n; i++) v[i] *= s;
}

static const float *cb_for(const thim_needle *n, int bits){ return n->cb[bits == 2 ? 0 : bits == 3 ? 1 : 2]; }

/* one row of a CQ tensor rebuilt in float: (cb[idx] * norm) @ H per group of 128 */
static void cq_row(const thim_needle *n, const uint8_t *packed, const uint16_t *norms, int bits, int group, int in_pad, int cols, int row, float *dst, float *tmp){
    const uint8_t *pr = packed + (size_t)row * (size_t)in_pad * (size_t)bits / 8;
    const uint16_t *nr = norms + (size_t)row * (size_t)(in_pad / group);
    const float *cb = cb_for(n, bits);
    int mask = (1 << bits) - 1;
    for(int g = 0; g < in_pad / group; g++){
        float nm = f16(nr[g]);
        for(int k = 0; k < group; k++){
            size_t bit = (size_t)(g * group + k) * (size_t)bits;
            uint32_t w = pr[bit >> 3] | (uint32_t)((bit >> 3) + 1 < (size_t)in_pad * bits / 8 ? pr[(bit >> 3) + 1] : 0) << 8;
            tmp[k] = cb[(w >> (bit & 7)) & mask] * nm;
        }
        fwht(tmp, group);
        for(int k = 0; k < group && g * group + k < cols; k++) dst[g * group + k] = tmp[k];
    }
}

/* float row -> int8 blocks of 32 with one scale each (zero padded to cp) */
static void q8_row(const float *src, int cols, int cp, int8_t *q, float *sc){
    for(int b = 0; b < cp / BLOCK; b++){
        float amax = 0.f;
        for(int i = 0; i < BLOCK; i++){ int c = b * BLOCK + i; float v = c < cols ? src[c] : 0.f; v = v < 0 ? -v : v; if(v > amax) amax = v; }
        float s = amax > 0.f ? amax / 127.f : 1.f, inv = amax > 0.f ? 127.f / amax : 0.f;
        sc[b] = s;
        for(int i = 0; i < BLOCK; i++){ int c = b * BLOCK + i; float v = c < cols ? src[c] * inv : 0.f; q[b * BLOCK + i] = (int8_t)(int)rne(v); }
    }
}

typedef struct { thim_needle *n; int cb_n, idx, bad; float *row, *tmp; } loader_t;

static int ld_next(loader_t *L, rec_t *r){
    if(L->bad) return 0;
    if(!read_rec(L->n, L->cb_n, L->idx++, r)){ L->bad = 1; return 0; }
    return 1;
}
static float *ld_f(loader_t *L, int want){            /* an FP16 or FP32 tensor as floats */
    rec_t r;
    if(!ld_next(L, &r)) return NULL;
    int cnt = 1;
    for(int k = 0; k < r.ndim; k++) cnt *= r.shape[k];
    if(want && cnt != want){ L->bad = 1; return NULL; }
    float *f = xalloc((size_t)cnt * sizeof(float));
    if(!f){ L->bad = 1; return NULL; }
    const uint8_t *p = L->n->base + r.off;
    if(r.dtype == 1) for(int i = 0; i < cnt; i++){ uint16_t h; memcpy(&h, p + 2 * i, 2); f[i] = f16(h); }
    else if(r.dtype == 2) memcpy(f, p, (size_t)cnt * 4);
    else { L->bad = 1; return f; }
    return f;
}
/* a CQ tensor appended as rows [row0, row0 + out) of an int8 matrix */
static int ld_cq_into(loader_t *L, mat_t *m, int row0, const int *rows_sel, int n_sel){
    rec_t r;
    if(!ld_next(L, &r) || r.dtype != 3){ L->bad = 1; return 0; }
    int out = r.shape[0], cols = r.shape[1], g = r.group, in_pad = (cols + g - 1) / g * g;
    if(cols != m->cols || g != 128){ L->bad = 1; return 0; }
    const uint8_t *packed = L->n->base + r.off;
    const uint16_t *norms = (const uint16_t *)(packed + (size_t)out * (size_t)in_pad * (size_t)r.bits / 8);
    int cnt = rows_sel ? n_sel : out;
    for(int i = 0; i < cnt; i++){
        int src = rows_sel ? rows_sel[i] : i;
        cq_row(L->n, packed, norms, r.bits, g, in_pad, cols, src, L->row, L->tmp);
        q8_row(L->row, cols, m->cp, m->q + (size_t)(row0 + i) * m->cp, m->sc + (size_t)(row0 + i) * (m->cp / BLOCK));
    }
    return 1;
}
static int mat_alloc(mat_t *m, int rows, int cols){
    m->rows = rows; m->cols = cols; m->cp = (cols + BLOCK - 1) / BLOCK * BLOCK;
    m->q = xalloc((size_t)rows * m->cp); m->sc = xalloc((size_t)rows * (m->cp / BLOCK) * sizeof(float));
    return m->q && m->sc;
}
static void mat_free(mat_t *m){ xfree(m->q); xfree(m->sc); m->q = NULL; m->sc = NULL; }

/* activations -> int8 with ONE scale per row (the archive's A8: absmax / 127, round half even) */
static int ensure_q(thim_needle *n, size_t M, size_t cp){
    size_t need = M * cp;
    if(need <= n->qa_cap) return 1;
    xfree(n->qa); xfree(n->qs);
    n->qa = xalloc(need); n->qs = xalloc(need / BLOCK * sizeof(float) + 64);
    n->qa_cap = n->qa && n->qs ? need : 0;
    return n->qa_cap != 0;
}
static void quant_a8(const float *A, int M, int K, int cp, int8_t *q, float *sc){
    int nb = cp / BLOCK;
    for(int m = 0; m < M; m++){
        const float *a = A + (size_t)m * K;
        float amax = 0.f;
        for(int i = 0; i < K; i++){ float v = a[i] < 0 ? -a[i] : a[i]; if(v > amax) amax = v; }
        float s = amax > 0.f ? amax / 127.f : 1.f;
        int8_t *qr = q + (size_t)m * cp;
        for(int i = 0; i < K; i++){ float r = rne(a[i] / s); if(r > 127.f) r = 127.f; if(r < -128.f) r = -128.f; qr[i] = (int8_t)(int)r; }
        for(int i = K; i < cp; i++) qr[i] = 0;
        for(int b = 0; b < nb; b++) sc[(size_t)m * nb + b] = s;
    }
}
/* the same rounding in place (for values that are not multiplied by a matrix: q, k, v) */
static void fake_a8(float *a, int K){
    float amax = 0.f;
    for(int i = 0; i < K; i++){ float v = a[i] < 0 ? -a[i] : a[i]; if(v > amax) amax = v; }
    float s = amax > 0.f ? amax / 127.f : 1.f;
    for(int i = 0; i < K; i++){ float r = rne(a[i] / s); if(r > 127.f) r = 127.f; if(r < -128.f) r = -128.f; a[i] = r * s; }
}

/* out[M][N] = A8(A[M][K]) W^T */
static void mm(thim_needle *n, float *out, const float *A, int M, const mat_t *W){
    int K = W->cols, cp = W->cp, N = W->rows, nb = cp / BLOCK;
    if(!ensure_q(n, (size_t)M + 4, (size_t)cp)) return;
    quant_a8(A, M, K, cp, n->qa, n->qs);
    if(M < 4){
        for(int m = 0; m < M; m++){
            const int8_t *ar = n->qa + (size_t)m * cp; const float *as = n->qs + (size_t)m * nb;
            for(int j = 0; j < N; j += 4){
                const int8_t *wp[4]; const float *sp[4]; float r[4];
                for(int i = 0; i < 4; i++){ int ni = j + i < N ? j + i : N - 1; wp[i] = W->q + (size_t)ni * cp; sp[i] = W->sc + (size_t)ni * nb; }
                rows4(wp, sp, ar, as, nb, r);
                for(int i = 0; i < 4 && j + i < N; i++) out[(size_t)m * N + j + i] = r[i];
            }
        }
        return;
    }
    for(int j = 0; j < N; j++){
        const int8_t *wr = W->q + (size_t)j * cp; const float *ws = W->sc + (size_t)j * nb;
        for(int m = 0; m < M; m += 4){
            const int8_t *ap[4]; const float *sp[4]; float r[4];
            for(int i = 0; i < 4; i++){ int mi = m + i < M ? m + i : M - 1; ap[i] = n->qa + (size_t)mi * cp; sp[i] = n->qs + (size_t)mi * nb; }
            tile4(wr, ws, ap, sp, nb, r);
            for(int i = 0; i < 4 && m + i < M; i++) out[(size_t)(m + i) * N + j] = r[i];
        }
    }
}

/* ---------------- layers ---------------- */
static void zc_norm(float *dst, const float *x, const float *scale, int D){
    float ss = 0.f;
    for(int i = 0; i < D; i++) ss += x[i] * x[i];
    float inv = 1.f / fsqrt(ss / (float)D + 1e-6f);
    for(int i = 0; i < D; i++) dst[i] = (1.f + scale[i]) * x[i] * inv;
}
static float rms_inv(const float *x, int D){
    float ss = 0.f;
    for(int i = 0; i < D; i++) ss += x[i] * x[i];
    return 1.f / fsqrt(ss / (float)D + 1e-6f);
}

/* the Kronecker factor: z (32x32, row major) -> A^T z B */
static void kron(const float *z, const float *a, const float *b, float *out, float *t){
    for(int i = 0; i < 32; i++){
        float *ti = t + i * 32;
        for(int l = 0; l < 32; l++) ti[l] = 0.f;
        for(int j = 0; j < 32; j++){ float zij = z[i * 32 + j]; const float *bj = b + j * 32; for(int l = 0; l < 32; l++) ti[l] += zij * bj[l]; }
    }
    for(int k = 0; k < 32; k++){ float *ok = out + k * 32; for(int l = 0; l < 32; l++) ok[l] = 0.f; }
    for(int i = 0; i < 32; i++){
        const float *ti = t + i * 32, *ai = a + i * 32;
        for(int k = 0; k < 32; k++){ float aik = ai[k]; float *ok = out + k * 32; for(int l = 0; l < 32; l++) ok[l] += aik * ti[l]; }
    }
}

static void hada(thim_needle *n, const layer_t *ly, const float *h, float *out){
    int D = n->d, N = n->hada_n;
    float *z = n->z, *t = n->zt, *cond = n->cond, *t2 = n->tmp;
    float c8[8], mx = -1e30f, sum = 0.f;
    for(int r = 0; r < 8; r++){ float s = 0.f; for(int i = 0; i < D; i++) s += h[i] * ly->cond_v[i * 8 + r]; c8[r] = s; if(s > mx) mx = s; }
    for(int r = 0; r < 8; r++){ c8[r] = fexp(c8[r] - mx); sum += c8[r]; }
    for(int r = 0; r < 8; r++) c8[r] /= sum;
    for(int j = 0; j < N; j++){ float s = 1.f; for(int r = 0; r < 8; r++) s += c8[r] * ly->cond_u[r * N + j]; cond[j] = s; }
    for(int j = 0; j < N; j++) t2[j] = j < D ? ly->d1[j] * h[j] : 0.f;
    kron(t2, ly->w[0], ly->w[1], t, z);
    for(int j = 0; j < N; j++) z[j] = t[n->p1[j]];
    for(int j = 0; j < N; j++){ float v = ly->d2[j] * cond[j] * z[j] + ly->b2[j]; t2[j] = v * sigm(v); }
    kron(t2, ly->w[2], ly->w[3], t, z);
    for(int j = 0; j < N; j++) z[j] = ly->d3[j] * t[n->p2[j]];
    kron(z, ly->w[4], ly->w[5], t, t2);
    for(int j = 0; j < D; j++) out[j] = ly->d4[j] * t[j];
}

/* ---------------- confidence accumulators (online softmax over positions) ---------------- */
static void conf_add(thim_needle *n, snap_t *s, int row, const float *cell){
    if(!n->has_head) return;
    int D = n->d;
    for(int k = 0; k < n->n_probe; k++){
        int i = row * n->n_probe + k;
        const float *pr = n->probes + (size_t)i * D;
        float sc = 0.f;
        for(int j = 0; j < D; j++) sc += cell[j] * pr[j];
        sc /= fsqrt((float)D);
        float *acc = s->cacc + (size_t)i * D;
        if(s->cs[i] == 0.f){ s->cm[i] = sc; s->cs[i] = 1.f; for(int j = 0; j < D; j++) acc[j] = cell[j]; }
        else if(sc > s->cm[i]){
            float f = fexp(s->cm[i] - sc);
            s->cs[i] = s->cs[i] * f + 1.f;
            for(int j = 0; j < D; j++) acc[j] = acc[j] * f + cell[j];
            s->cm[i] = sc;
        } else {
            float w = fexp(sc - s->cm[i]);
            s->cs[i] += w;
            for(int j = 0; j < D; j++) acc[j] += w * cell[j];
        }
    }
}
static float conf_head(thim_needle *n, const snap_t *s){
    if(!n->has_head) return -1.f;
    int D = n->d, R = n->L + 1, K = n->n_probe, Q = n->n_query, RK = R * K;
    float *r = n->tmp;                       /* RK * D floats are needed: use the hada scratch? no: allocate */
    r = xalloc((size_t)RK * D * sizeof(float));
    float *u = xalloc((size_t)Q * RK * sizeof(float));
    if(!r || !u){ xfree(r); xfree(u); return -1.f; }
    for(int i = 0; i < RK; i++){
        float *ri = r + (size_t)i * D;
        for(int j = 0; j < D; j++) ri[j] = s->cacc[(size_t)i * D + j] / s->cs[i];
        float inv = rms_inv(ri, D), g = n->gain[i];
        for(int j = 0; j < D; j++) ri[j] *= inv * g;
    }
    float logit = n->proj_bias;
    for(int q = 0; q < Q; q++){
        float mx = -1e30f, sum = 0.f;
        for(int i = 0; i < RK; i++){
            float d = 0.f; const float *ri = r + (size_t)i * D, *qq = n->query + (size_t)q * D;
            for(int j = 0; j < D; j++) d += ri[j] * qq[j];
            /* row_bias is [Q][R][K]: i = row * K + k */
            u[q * RK + i] = d / fsqrt((float)D) + n->row_bias[(size_t)q * RK + i];
            if(u[q * RK + i] > mx) mx = u[q * RK + i];
        }
        for(int i = 0; i < RK; i++){ u[q * RK + i] = fexp(u[q * RK + i] - mx); sum += u[q * RK + i]; }
        for(int j = 0; j < D; j++){
            float p = 0.f;
            for(int i = 0; i < RK; i++) p += u[q * RK + i] * r[(size_t)i * D + j];
            logit += (p / sum) * n->proj[(size_t)q * D + j];
        }
    }
    xfree(r); xfree(u);
    return sigm(logit);
}

/* ---------------- one batch of positions ---------------- */
static int is_global(const thim_needle *n, int l){ return l < 32 ? (n->gmask_lo >> l) & 1 : (n->gmask_hi >> (l - 32)) & 1; }

static void emb_row(thim_needle *n, int tok, float *dst){
    cq_row(n, n->emb_packed, n->emb_norms, n->emb_bits, 128, n->d, n->d, tok, dst, n->tmp);
}

static uint32_t eg_hash(const thim_needle *n, int oi, int h, int t){
    uint32_t acc = 0x9E3779B9u * (uint32_t)(oi * (n->eg_tables / n->n_orders) + h + 1);
    for(int j = 0; j < n->orders[oi]; j++){
        uint32_t u = t - j >= 0 ? (uint32_t)n->tokens[t - j] : 0u;
        acc = (acc ^ u) * 0x01000193u;
    }
    acc ^= acc >> 15;
    return acc % (uint32_t)n->eg_slots;
}

/* positions [t0, t0 + M): tokens already in n->tokens. Logits of the last into n->logits if want. */
static void forward(thim_needle *n, int t0, int M, int want_logits){
    int D = n->d, NL = n->lanes, L = n->L, QW = n->nh * n->qk, KW = n->nkv * n->qk, VW = n->nkv * n->vd, QKV = n->qkv;
    float *X = n->X;
    snap_t *s = &n->cur;
    /* embeddings */
    for(int m = 0; m < M; m++){
        float *x0 = n->x0 + (size_t)m * D;
        emb_row(n, n->tokens[t0 + m], x0);
        float sc = fsqrt((float)D);
        for(int j = 0; j < D; j++) x0[j] *= sc;
        for(int l = 0; l < NL; l++) memcpy(X + ((size_t)m * NL + l) * D, x0, (size_t)D * 4);
        conf_add(n, s, 0, x0);
    }
    /* engram keys and values for every site */
    int per_order = n->eg_tables / n->n_orders;
    for(int si = 0; si < n->n_sites; si++){
        site_t *st = &n->site[si];
        for(int m = 0; m < M; m++){
            int t = t0 + m;
            float *e = n->e + (size_t)m * D;
            int ti = 0;
            for(int oi = 0; oi < n->n_orders; oi++)
                for(int h = 0; h < per_order; h++, ti++){
                    float *dst = e + ti * n->eg_sub;
                    if(t < n->orders[oi] - 1){ memset(dst, 0, (size_t)n->eg_sub * 4); continue; }
                    uint32_t slot = eg_hash(n, oi, h, t);
                    cq_row(n, st->tab_packed, st->tab_norms, st->tab_bits, 128, n->eg_sub, n->eg_sub,
                           (int)((uint32_t)ti * (uint32_t)n->eg_slots + slot), dst, n->tmp);
                }
        }
        mm(n, n->ekv, n->e, M, &st->kv);
        /* conv taps over the raw value rows, dilation eg_dil; history holds t-1 .. t-9 */
        for(int m = 0; m < M; m++){
            int t = t0 + m;
            float *ek = n->ek + ((size_t)si * CH + m) * D, *ev = n->ev + ((size_t)si * CH + m) * D;
            memcpy(ek, n->ekv + (size_t)m * 2 * D, (size_t)D * 4);
            for(int j = 0; j < D; j++) ev[j] = 0.f;
            for(int k = 0; k < n->eg_ctaps; k++){
                int back = k * n->eg_dil, src = t - back;
                if(src < 0) continue;
                const float *vr;
                if(m - back >= 0) vr = n->ekv + (size_t)(m - back) * 2 * D + D;
                else vr = s->ev_hist + ((size_t)si * 9 + (size_t)(back - m - 1)) * D;
                const float *tp = st->taps + (size_t)k * D;
                for(int j = 0; j < D; j++) ev[j] += tp[j] * vr[j];
            }
        }
        /* history: the last nine raw value rows, newest first */
        float keep[9 * 768];
        for(int b = 0; b < 9; b++){
            int m = M - 1 - b;
            const float *src = m >= 0 ? n->ekv + (size_t)m * 2 * D + D : s->ev_hist + ((size_t)si * 9 + (size_t)(b - M)) * D;
            memcpy(keep + (size_t)b * D, src, (size_t)D * 4);
        }
        memcpy(s->ev_hist + (size_t)si * 9 * D, keep, (size_t)9 * D * 4);
    }

    for(int l = 0; l < L; l++){
        layer_t *ly = &n->ly[l];
        int lane = l % NL;
        /* lane maps */
        for(int m = 0; m < M; m++){
            const float *xm = X + (size_t)m * NL * D;
            float inv = rms_inv(xm, NL * D);
            float *nx = n->nx + (size_t)m * NL * D;
            for(int j = 0; j < NL * D; j++) nx[j] = xm[j] * inv;
        }
        mm(n, n->mo, n->nx, M, &ly->phi);
        for(int m = 0; m < M; m++){
            const float *o = n->mo + (size_t)m * 24;
            const float *xm = X + (size_t)m * NL * D;
            float *u = n->u + (size_t)m * D;
            float hp[4];
            for(int i = 0; i < NL; i++) hp[i] = sigm(n->a_pre[l] * o[i] + n->b_pre[l * NL + i] + (i == lane ? 4.f : -4.f));
            for(int j = 0; j < D; j++){ float v = 0.f; for(int i = 0; i < NL; i++) v += hp[i] * xm[i * D + j]; u[j] = v; }
        }
        /* the block on u: engram, attention, MLP; y = block(u) - u is kept in hb */
        int si = n->site_of_layer[l];
        for(int m = 0; m < M; m++){
            float *u = n->u + (size_t)m * D, *x = n->hb + (size_t)m * D;
            memcpy(x, u, (size_t)D * 4);
            if(si >= 0){
                const float *ek = n->ek + ((size_t)si * CH + m) * D, *ev = n->ev + ((size_t)si * CH + m) * D;
                float a = rms_inv(x, D), b = rms_inv(ek, D), d = 0.f;
                for(int j = 0; j < D; j++) d += (x[j] * a) * (ek[j] * b);
                float alpha = sigm(d / fsqrt((float)D));
                for(int j = 0; j < D; j++) x[j] += alpha * ev[j];
            }
            zc_norm(n->att + (size_t)m * D, x, ly->norm_in, D);
        }
        /* q, k, v and the gate from one product */
        mm(n, n->raw, n->att, M, &ly->qkvg);
        for(int m = 0; m < M; m++){
            int t = t0 + m;
            const float *rw = n->raw + (size_t)m * (QKV + D);
            float *q = n->qb, *k = n->kb, *v = n->vb;
            /* causal conv over raw q|k|v with taps; history rows are t-1 and t-2 */
            for(int c = 0; c < QKV; c++){
                const float *tp = c < QW ? ly->q_taps + c : c < QW + KW ? ly->k_taps + (c - QW) : ly->v_taps + (c - QW - KW);
                int stride = c < QW ? QW : c < QW + KW ? KW : VW;
                float acc = 0.f;
                for(int j = 0; j < n->taps; j++){
                    if(t - j < 0) break;
                    float rv = m - j >= 0 ? n->raw[(size_t)(m - j) * (QKV + D) + c] : s->raw_hist[((size_t)l * 2 + (size_t)(j - m - 1)) * QKV + c];
                    acc += tp[(size_t)j * stride] * rv;
                }
                if(c < QW) q[c] = acc; else if(c < QW + KW) k[c - QW] = acc; else v[c - QW - KW] = acc;
            }
            /* norms, rope, int8 rounding per head */
            int half = n->qk / 2;
            const float *cs = n->cosb + (size_t)t * half, *sn = n->sinb + (size_t)t * half;
            for(int h = 0; h < n->nh + n->nkv; h++){
                float *hv = h < n->nh ? q + h * n->qk : k + (h - n->nh) * n->qk;
                float tmp[64];
                zc_norm(tmp, hv, h < n->nh ? ly->q_norm : ly->k_norm, n->qk);
                for(int i = 0; i < half; i++){
                    float x1 = tmp[i], x2 = tmp[i + half];
                    hv[i] = x1 * cs[i] - x2 * sn[i];
                    hv[i + half] = x2 * cs[i] + x1 * sn[i];
                }
                fake_a8(hv, n->qk);
            }
            for(int h = 0; h < n->nkv; h++) fake_a8(v + h * n->vd, n->vd);
            float *Kt = n->K + ((size_t)l * MAXT + t) * KW, *Vt = n->V + ((size_t)l * MAXT + t) * VW;
            memcpy(Kt, k, (size_t)KW * 4); memcpy(Vt, v, (size_t)VW * 4);
            /* attention over [s0, t] */
            int s0 = 0;
            if(!is_global(n, l) && n->window > 0 && t - n->window + 1 > 0) s0 = t - n->window + 1;
            float *out = n->att + (size_t)m * D;          /* att row m is free again (raw holds what it fed) */
            float scale = 1.f / fsqrt((float)n->qk);
            int rep = n->nh / n->nkv, S = t - s0 + 1;
            for(int h = 0; h < n->nh; h++){
                int kvh = h / rep;
                const float *qh = q + h * n->qk;
                float *sc = n->sc, mx = -1e30f, sum = 0.f;
                for(int si2 = 0; si2 < S; si2++){
                    const float *kk = n->K + ((size_t)l * MAXT + s0 + si2) * KW + kvh * n->qk;
                    float d = 0.f;
                    for(int i = 0; i < n->qk; i++) d += qh[i] * kk[i];
                    d *= scale; sc[si2] = d; if(d > mx) mx = d;
                }
                for(int si2 = 0; si2 < S; si2++){ sc[si2] = fexp(sc[si2] - mx); sum += sc[si2]; }
                float *oh = out + h * n->vd, inv = 1.f / sum;
                for(int i = 0; i < n->vd; i++) oh[i] = 0.f;
                for(int si2 = 0; si2 < S; si2++){
                    const float *vv = n->V + ((size_t)l * MAXT + s0 + si2) * VW + kvh * n->vd;
                    float p = sc[si2] * inv;
                    for(int i = 0; i < n->vd; i++) oh[i] += p * vv[i];
                }
            }
            const float *gate = rw + QKV;
            for(int j = 0; j < D; j++) out[j] *= sigm(gate[j]);
        }
        /* conv history for the next batch: raw rows of the last two positions */
        for(int b = 1; b >= 0; b--){                       /* oldest first: b = 1 may read the old b = 0 */
            int m = M - 1 - b;
            float *dst = s->raw_hist + ((size_t)l * 2 + b) * QKV;
            if(m >= 0) memcpy(dst, n->raw + (size_t)m * (QKV + D), (size_t)QKV * 4);
            else memmove(dst, s->raw_hist + ((size_t)l * 2 + (size_t)(b - M)) * QKV, (size_t)QKV * 4);
        }
        mm(n, n->e, n->att, M, &ly->out);                 /* e: attention output rows */
        for(int m = 0; m < M; m++){
            float *x = n->hb + (size_t)m * D, *a = n->e + (size_t)m * D, tmp[768];
            zc_norm(tmp, a, ly->post_norm, D);
            float g = sigm(ly->attn_gate);
            for(int j = 0; j < D; j++) x[j] += g * tmp[j];
            zc_norm(tmp, x, ly->pre_hada, D);
            float mlp[768];
            hada(n, ly, tmp, mlp);
            const float *u = n->u + (size_t)m * D;
            for(int j = 0; j < D; j++) x[j] = x[j] + mlp[j] - u[j];          /* y = block(u) - u */
        }
        /* lane update: X_i = sum_j hres[i][j] X_j + hpost_i * y */
        for(int m = 0; m < M; m++){
            const float *o = n->mo + (size_t)m * 24, *y = n->hb + (size_t)m * D;
            float *xm = X + (size_t)m * NL * D;
            float hpost[4], K4[16], mx = -1e30f;
            for(int i = 0; i < NL; i++) hpost[i] = 2.f * sigm(n->a_post[l] * o[4 + i] + n->b_post[l * NL + i] + (i == lane ? 0.f : -4.f));
            for(int i = 0; i < 16; i++){ K4[i] = n->a_res[l] * o[8 + i] + n->b_res[l * 16 + i]; if(K4[i] > mx) mx = K4[i]; }
            /* Sinkhorn in the log domain, 20 rounds of row then column normalisation */
            for(int it = 0; it < 20; it++){
                for(int i = 0; i < 4; i++){
                    float rm = K4[i * 4]; for(int j = 1; j < 4; j++) if(K4[i * 4 + j] > rm) rm = K4[i * 4 + j];
                    float ss = 0.f; for(int j = 0; j < 4; j++) ss += fexp(K4[i * 4 + j] - rm);
                    float lse = rm + flog(ss);
                    for(int j = 0; j < 4; j++) K4[i * 4 + j] -= lse;
                }
                for(int j = 0; j < 4; j++){
                    float cm = K4[j]; for(int i = 1; i < 4; i++) if(K4[i * 4 + j] > cm) cm = K4[i * 4 + j];
                    float ss = 0.f; for(int i = 0; i < 4; i++) ss += fexp(K4[i * 4 + j] - cm);
                    float lse = cm + flog(ss);
                    for(int i = 0; i < 4; i++) K4[i * 4 + j] -= lse;
                }
            }
            for(int i = 0; i < 16; i++) K4[i] = fexp(K4[i]);
            (void)mx;
            float nxm[4 * 768];
            for(int i = 0; i < NL; i++)
                for(int j = 0; j < D; j++){
                    float v = hpost[i] * y[j];
                    for(int k = 0; k < NL; k++) v += K4[i * 4 + k] * xm[k * D + j];
                    nxm[i * D + j] = v;
                }
            memcpy(xm, nxm, (size_t)NL * D * 4);
            float cell[768];
            for(int j = 0; j < D; j++){ float v = 0.f; for(int i = 0; i < NL; i++) v += xm[i * D + j]; cell[j] = v / (float)NL; }
            conf_add(n, s, l + 1, cell);
        }
    }
    if(want_logits){
        const float *xm = X + (size_t)(M - 1) * NL * D;
        float x[768], xn[768];
        for(int j = 0; j < D; j++){ float v = 0.f; for(int i = 0; i < NL; i++) v += xm[i * D + j]; x[j] = v / (float)NL; }
        zc_norm(xn, x, n->final_norm, D);
        mm(n, n->logits, xn, 1, &n->emb);
    }
}

/* feeds tokens in batches of CH; logits of the last one */
static int feed(thim_needle *n, const int *ids, int count, int want_logits){
    if(n->n + count > MAXT) return -1;
    for(int i = 0; i < count; i++) n->tokens[n->n + i] = ids[i];
    for(int i = 0; i < count; i += CH){
        int M = count - i < CH ? count - i : CH;
        forward(n, n->n + i, M, want_logits && i + M == count);
    }
    n->n += count;
    n->cur.len = n->n;
    return 0;
}

/* ---------------- tokenizer (the archive's SentencePiece BPE; export.py RefTokenizer) ---------------- */
static uint32_t fnv(const char *s, int len){ uint32_t h = 2166136261u; for(int i = 0; i < len; i++){ h ^= (uint8_t)s[i]; h *= 16777619u; } return h; }
static int piece_id(const thim_needle *n, const char *s, int len){
    uint32_t h = fnv(s, len) & (uint32_t)(n->hash_cap - 1);
    for(;;){
        int id = n->hash[h];
        if(id < 0) return -1;
        if(n->piece_len[id] == len && !memcmp(n->piece[id], s, (size_t)len)) return id;
        h = (h + 1) & (uint32_t)(n->hash_cap - 1);
    }
}
static int utf8_len(uint8_t c){ return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

/* BPE over one segment (already escaped): symbols are code points, merged by best score */
static int bpe(const thim_needle *n, const char *seg, int len, int *ids, int cap, int nout){
    if(len <= 0) return nout;
    int *st = xalloc((size_t)(len + 1) * sizeof(int)), *ln = xalloc((size_t)(len + 1) * sizeof(int));
    if(!st || !ln){ xfree(st); xfree(ln); return nout; }
    int ns = 0;
    for(int i = 0; i < len; ){ int l = utf8_len((uint8_t)seg[i]); if(i + l > len) l = len - i; st[ns] = i; ln[ns] = l; ns++; i += l; }
    while(ns > 1){
        float best = 0.f; int bj = -1;
        for(int j = 0; j + 1 < ns; j++){
            int id = piece_id(n, seg + st[j], ln[j] + ln[j + 1]);
            if(id >= 0 && (bj < 0 || n->score[id] > best)){ best = n->score[id]; bj = j; }
        }
        if(bj < 0) break;
        ln[bj] += ln[bj + 1];
        for(int j = bj + 1; j + 1 < ns; j++){ st[j] = st[j + 1]; ln[j] = ln[j + 1]; }
        ns--;
    }
    for(int j = 0; j < ns; j++){
        int id = piece_id(n, seg + st[j], ln[j]);
        if(id >= 0){ if(nout < cap) ids[nout] = id; nout++; }
        else for(int b = 0; b < ln[j]; b++){ if(nout < cap) ids[nout] = n->byte_id[(uint8_t)seg[st[j] + b]]; nout++; }
    }
    xfree(st); xfree(ln);
    return nout;
}

int thim_needle_encode(thim_needle *n, const char *text, int *ids, int cap){
    int tl = (int)strlen(text);
    char *esc = xalloc((size_t)tl * 3 + 8);
    if(!esc) return -1;
    int el = 0;
    for(int i = 0; i < tl; i++){
        if(text[i] == ' '){ esc[el++] = (char)0xE2; esc[el++] = (char)0x96; esc[el++] = (char)0x81; }
        else esc[el++] = text[i];
    }
    int nout = 0, seg0 = 0;
    for(int i = 0; i < el; ){
        int hit = -1;
        for(int k = 0; k < n->n_markers; k++){
            int id = n->markers[k], l = n->piece_len[id];
            if(i + l <= el && !memcmp(esc + i, n->piece[id], (size_t)l)){ hit = id; break; }
        }
        if(hit >= 0){
            nout = bpe(n, esc + seg0, i - seg0, ids, cap, nout);
            if(nout < cap) ids[nout] = hit;
            nout++;
            i += n->piece_len[hit]; seg0 = i;
        } else i++;
    }
    nout = bpe(n, esc + seg0, el - seg0, ids, cap, nout);
    xfree(esc);
    return nout;
}

int thim_needle_vocab(const thim_needle *n){ return n->vocab; }
const char *thim_needle_piece(const thim_needle *n, int id){ return id >= 0 && id < n->n_pieces ? n->piece[id] : ""; }

/* what token id adds to the text: 0 markers/controls (*marker set), else bytes in out */
static int token_bytes(const thim_needle *n, int id, char *out, int *marker){
    *marker = 0;
    if(id < 0 || id >= n->n_pieces){ *marker = 1; return 0; }
    int t = n->ptype[id];
    if(t == 4){ unsigned v = 0; const char *p = n->piece[id] + 3; for(int i = 0; i < 2; i++){ char c = p[i]; v = v * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); } out[0] = (char)v; return 1; }
    if(t == 1 || t == 2 || t == 3){ *marker = 1; return 0; }
    int k = 0, l = n->piece_len[id];
    const char *p = n->piece[id];
    for(int i = 0; i < l; ){
        if(i + 2 < l && (uint8_t)p[i] == 0xE2 && (uint8_t)p[i + 1] == 0x96 && (uint8_t)p[i + 2] == 0x81){ out[k++] = ' '; i += 3; }
        else out[k++] = p[i++];
    }
    return k;
}

static int load_tokenizer(thim_needle *n, const uint8_t *b, size_t len){
    if(len < 24) return 0;
    n->n_pieces = (int)rd32(b);
    n->bos = (int)rd32(b + 12); n->eos = (int)rd32(b + 8); n->unk = (int)rd32(b + 16);
    size_t off = 24;
    n->piece = xalloc((size_t)n->n_pieces * sizeof(char *));
    n->piece_len = xalloc((size_t)n->n_pieces * 2);
    n->score = xalloc((size_t)n->n_pieces * 4);
    n->ptype = xalloc((size_t)n->n_pieces);
    if(!n->piece || !n->piece_len || !n->score || !n->ptype) return 0;
    for(int i = 0; i < 256; i++) n->byte_id[i] = n->unk;
    for(int i = 0; i < n->n_pieces; i++){
        if(off + 7 > len) return 0;
        float sc; memcpy(&sc, b + off, 4);
        uint16_t pl; memcpy(&pl, b + off + 5, 2);
        n->score[i] = sc; n->ptype[i] = b[off + 4]; n->piece_len[i] = pl;
        off += 7;
        if(off + pl > len) return 0;
        char *s = xalloc((size_t)pl + 1);
        if(!s) return 0;
        memcpy(s, b + off, pl); s[pl] = 0;
        n->piece[i] = s;
        off += pl;
    }
    n->hash_cap = 1; while(n->hash_cap < n->n_pieces * 2) n->hash_cap <<= 1;
    n->hash = xalloc((size_t)n->hash_cap * sizeof(int));
    if(!n->hash) return 0;
    for(int i = 0; i < n->hash_cap; i++) n->hash[i] = -1;
    n->n_markers = 0;
    for(int i = 0; i < n->n_pieces; i++){
        uint32_t h = fnv(n->piece[i], n->piece_len[i]) & (uint32_t)(n->hash_cap - 1);
        while(n->hash[h] >= 0) h = (h + 1) & (uint32_t)(n->hash_cap - 1);
        n->hash[h] = i;
        if(n->ptype[i] == 4){ const char *p = n->piece[i] + 3; unsigned v = 0; for(int k = 0; k < 2; k++){ char c = p[k]; v = v * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); } n->byte_id[v & 255] = i; }
        if(n->ptype[i] == 3 && n->n_markers < 32) n->markers[n->n_markers++] = i;
    }
    /* markers longest first */
    for(int i = 0; i < n->n_markers; i++)
        for(int j = i + 1; j < n->n_markers; j++)
            if(n->piece_len[n->markers[j]] > n->piece_len[n->markers[i]]){ int t = n->markers[i]; n->markers[i] = n->markers[j]; n->markers[j] = t; }
    n->tk_think = piece_id(n, "<think>", 7); n->tk_think_end = piece_id(n, "</think>", 8);
    n->tk_call = piece_id(n, "<tool_call>", 11); n->tk_call_end = piece_id(n, "</tool_call>", 12);
    n->tk_im_end = piece_id(n, "<|im_end|>", 10); n->tk_im_start = piece_id(n, "<|im_start|>", 12);
    return n->tk_call >= 0 && n->tk_call_end >= 0 && n->tk_im_end >= 0;
}

/* ---------------- open / close ---------------- */
static int snap_alloc(thim_needle *n, snap_t *s){
    s->raw_hist = xalloc((size_t)n->L * 2 * n->qkv * 4);
    s->ev_hist = xalloc((size_t)(n->n_sites ? n->n_sites : 1) * 9 * n->d * 4);
    size_t rk = (size_t)(n->L + 1) * (n->n_probe ? n->n_probe : 1);
    s->cm = xalloc(rk * 4); s->cs = xalloc(rk * 4); s->cacc = xalloc(rk * n->d * 4);
    return s->raw_hist && s->ev_hist && s->cm && s->cs && s->cacc;
}
static void snap_free(snap_t *s){ xfree(s->raw_hist); xfree(s->ev_hist); xfree(s->cm); xfree(s->cs); xfree(s->cacc); }
static void snap_copy(const thim_needle *n, snap_t *dst, const snap_t *src){
    dst->len = src->len;
    memcpy(dst->raw_hist, src->raw_hist, (size_t)n->L * 2 * n->qkv * 4);
    memcpy(dst->ev_hist, src->ev_hist, (size_t)(n->n_sites ? n->n_sites : 1) * 9 * n->d * 4);
    size_t rk = (size_t)(n->L + 1) * (n->n_probe ? n->n_probe : 1);
    memcpy(dst->cm, src->cm, rk * 4); memcpy(dst->cs, src->cs, rk * 4); memcpy(dst->cacc, src->cacc, rk * n->d * 4);
}
static void snap_clear(thim_needle *n, snap_t *s){
    s->len = 0;
    memset(s->raw_hist, 0, (size_t)n->L * 2 * n->qkv * 4);
    memset(s->ev_hist, 0, (size_t)(n->n_sites ? n->n_sites : 1) * 9 * n->d * 4);
    size_t rk = (size_t)(n->L + 1) * (n->n_probe ? n->n_probe : 1);
    memset(s->cm, 0, rk * 4); memset(s->cs, 0, rk * 4);
}

static void gram_free(struct gram *g);

void thim_needle_close(thim_needle *n){
    if(!n) return;
    mat_free(&n->emb);
    for(int l = 0; l < n->L && l < MAXL; l++){
        layer_t *ly = &n->ly[l];
        mat_free(&ly->qkvg); mat_free(&ly->out); mat_free(&ly->phi);
        float **fs[] = { &ly->norm_in, &ly->q_taps, &ly->k_taps, &ly->v_taps, &ly->q_norm, &ly->k_norm, &ly->post_norm, &ly->pre_hada,
                         &ly->d1, &ly->d2, &ly->b2, &ly->d3, &ly->d4, &ly->w[0], &ly->w[1], &ly->w[2], &ly->w[3], &ly->w[4], &ly->w[5], &ly->cond_v, &ly->cond_u };
        for(size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) xfree(*fs[i]);
    }
    for(int s = 0; s < n->n_sites; s++){ mat_free(&n->site[s].kv); xfree(n->site[s].taps); }
    xfree(n->b_pre); xfree(n->b_post); xfree(n->b_res); xfree(n->p1); xfree(n->p2); xfree(n->final_norm);
    xfree(n->probes); xfree(n->gain); xfree(n->query); xfree(n->row_bias); xfree(n->proj);
    if(n->piece) for(int i = 0; i < n->n_pieces; i++) xfree((void *)n->piece[i]);
    xfree((void *)n->piece); xfree(n->piece_len); xfree(n->score); xfree(n->ptype); xfree(n->hash);
    xfree(n->tokens); xfree(n->K); xfree(n->V); snap_free(&n->cur); snap_free(&n->pre);
    xfree(n->cosb); xfree(n->sinb); xfree(n->qa); xfree(n->qs);
    float **bufs[] = { &n->X, &n->x0, &n->u, &n->hb, &n->nx, &n->mo, &n->raw, &n->qb, &n->kb, &n->vb, &n->att, &n->e, &n->ekv, &n->ek, &n->ev, &n->sc, &n->z, &n->zt, &n->cond, &n->logits, &n->tmp };
    for(size_t i = 0; i < sizeof bufs / sizeof bufs[0]; i++) xfree(*bufs[i]);
    xfree(n->tools_json); xfree(n->system); xfree(n->tools_lc);
    gram_free(n->g);
    xfree(n);
}

thim_needle *thim_needle_open(const void *data, size_t len, char *err, size_t err_len){
    const uint8_t *b = data;
    if(!b || len < HDR_BYTES || rd32(b) != TAG){ copy_err(err, err_len, "not a Needle 3 .cact archive"); return NULL; }
    thim_needle *n = xalloc(sizeof *n);
    if(!n){ copy_err(err, err_len, "out of memory"); return NULL; }
    n->base = b; n->len = len;
    uint32_t h[48]; for(int i = 0; i < 48; i++) h[i] = rd32(b + 4 * i);
    int num_t = (int)h[1], cb_n = (int)h[2];
    n->kv_window = (int)h[3]; n->vocab = (int)h[5]; n->out_vocab = h[6] ? (int)h[6] : (int)h[5];
    n->d = (int)h[7]; n->nh = (int)h[8]; n->nkv = (int)h[9]; n->L = (int)h[10]; n->qk = (int)h[11]; n->vd = (int)h[12];
    n->hada_n = (int)h[14]; n->lanes = (int)h[15]; n->window = (int)h[16]; n->gmask_lo = h[17]; n->gmask_hi = h[18];
    n->taps = (int)h[19]; n->eg_slots = (int)h[20]; n->eg_sub = (int)h[21]; n->eg_tables = (int)h[22];
    n->eg_ctaps = (int)h[23]; n->eg_dil = (int)h[24]; n->n_orders = (int)h[26];
    for(int i = 0; i < 4; i++) n->orders[i] = (int)h[27 + i];
    n->n_sites = (int)h[31];
    memcpy(&n->rope_theta, b + 192, 4);
    if(n->d != 768 || n->lanes != 4 || n->hada_n != 1024 || n->L > MAXL || n->n_sites > MAXSITE || cb_n != 28 || n->qk > 64 || n->vd > 64
       || n->n_orders < 1 || n->n_orders > 4 || h[25] != 0 || n->taps > 3 || n->eg_ctaps * n->eg_dil > 12){
        copy_err(err, err_len, "an archive geometry this engine was not written for"); xfree(n); return NULL;
    }
    for(int i = 0; i < 4; i++){ float v; memcpy(&v, b + HDR_BYTES + 4 * i, 4); n->cb[0][i] = v; }
    for(int i = 0; i < 8; i++){ float v; memcpy(&v, b + HDR_BYTES + 16 + 4 * i, 4); n->cb[1][i] = v; }
    for(int i = 0; i < 16; i++){ float v; memcpy(&v, b + HDR_BYTES + 48 + 4 * i, 4); n->cb[2][i] = v; }
    for(int l = 0; l < MAXL; l++) n->site_of_layer[l] = -1;
    for(int s = 0; s < n->n_sites; s++){ int l = (int)h[32 + s]; n->site[s].layer = l; if(l >= 0 && l < MAXL) n->site_of_layer[l] = s; }
    int D = n->d, QW = n->nh * n->qk, KW = n->nkv * n->qk, VW = n->nkv * n->vd;
    n->qkv = QW + KW + VW;

    loader_t L = { n, cb_n, 0, 0, xalloc(4096 * 4), xalloc(4096 * 4) };
    if(!L.row || !L.tmp){ copy_err(err, err_len, "out of memory"); thim_needle_close(n); return NULL; }
    n->tmp = xalloc(4096 * 4);
    /* embedding: packed for lookups, int8 for the head */
    rec_t r;
    if(!ld_next(&L, &r) || r.dtype != 3 || r.shape[1] != D){ L.bad = 1; }
    else {
        n->emb_packed = b + r.off; n->emb_bits = r.bits;
        n->emb_norms = (const uint16_t *)(b + r.off + (size_t)r.shape[0] * D * r.bits / 8);
        L.idx--;
        if(!mat_alloc(&n->emb, n->out_vocab, D)) L.bad = 1;
        else ld_cq_into(&L, &n->emb, 0, NULL, 0);
    }
    for(int l = 0; l < n->L && !L.bad; l++){
        layer_t *ly = &n->ly[l];
        ly->norm_in = ld_f(&L, D);
        if(!mat_alloc(&ly->qkvg, n->qkv + D, D)){ L.bad = 1; break; }
        ld_cq_into(&L, &ly->qkvg, 0, NULL, 0);
        ld_cq_into(&L, &ly->qkvg, QW, NULL, 0);
        ld_cq_into(&L, &ly->qkvg, QW + KW, NULL, 0);
        ly->q_taps = ld_f(&L, n->taps * QW); ly->k_taps = ld_f(&L, n->taps * KW); ly->v_taps = ld_f(&L, n->taps * VW);
        ly->q_norm = ld_f(&L, n->qk); ly->k_norm = ld_f(&L, n->qk);
        ld_cq_into(&L, &ly->qkvg, n->qkv, NULL, 0);                  /* gate */
        if(!mat_alloc(&ly->out, D, n->nh * n->vd)){ L.bad = 1; break; }
        ld_cq_into(&L, &ly->out, 0, NULL, 0);
        ly->post_norm = ld_f(&L, D);
        float *g = ld_f(&L, 1); ly->attn_gate = g ? g[0] : 0.f; xfree(g);
        ly->pre_hada = ld_f(&L, D);
        ly->d1 = ld_f(&L, n->hada_n); ly->d2 = ld_f(&L, n->hada_n); ly->b2 = ld_f(&L, n->hada_n);
        ly->d3 = ld_f(&L, n->hada_n); ly->d4 = ld_f(&L, n->hada_n);
        for(int k = 0; k < 6; k++) ly->w[k] = ld_f(&L, 32 * 32);
        ly->cond_v = ld_f(&L, D * 8); ly->cond_u = ld_f(&L, 8 * n->hada_n);
    }
    if(!L.bad){
        float *a;
        a = ld_f(&L, n->L); if(a){ memcpy(n->a_pre, a, (size_t)n->L * 4); xfree(a); }
        a = ld_f(&L, n->L); if(a){ memcpy(n->a_post, a, (size_t)n->L * 4); xfree(a); }
        a = ld_f(&L, n->L); if(a){ memcpy(n->a_res, a, (size_t)n->L * 4); xfree(a); }
        n->b_pre = ld_f(&L, n->L * 4); n->b_post = ld_f(&L, n->L * 4); n->b_res = ld_f(&L, n->L * 16);
        /* the three lane maps, rows l*lanes+lane (res: l*16+q), go to each layer's 24-row matrix */
        for(int l = 0; l < n->L && !L.bad; l++) if(!mat_alloc(&n->ly[l].phi, 24, 4 * D)) L.bad = 1;
        for(int which = 0; which < 3 && !L.bad; which++){
            int per = which < 2 ? 4 : 16;
            int idx0 = L.idx;
            for(int l = 0; l < n->L && !L.bad; l++){
                int sel[16];
                for(int k = 0; k < per; k++) sel[k] = l * per + k;
                L.idx = idx0;
                ld_cq_into(&L, &n->ly[l].phi, which == 0 ? 0 : which == 1 ? 4 : 8, sel, per);
            }
        }
        float *p = ld_f(&L, n->hada_n), *q = ld_f(&L, n->hada_n);
        n->p1 = xalloc((size_t)n->hada_n * sizeof(int)); n->p2 = xalloc((size_t)n->hada_n * sizeof(int));
        if(p && q && n->p1 && n->p2) for(int i = 0; i < n->hada_n; i++){
            n->p1[i] = (int)p[i]; n->p2[i] = (int)q[i];
            if(n->p1[i] < 0 || n->p1[i] >= n->hada_n || n->p2[i] < 0 || n->p2[i] >= n->hada_n) L.bad = 1;
        } else L.bad = 1;
        xfree(p); xfree(q);
        for(int s = 0; s < n->n_sites && !L.bad; s++){
            site_t *st = &n->site[s];
            if(!ld_next(&L, &r) || r.dtype != 3 || r.shape[1] != n->eg_sub || r.shape[0] != n->eg_tables * n->eg_slots || r.group != 128){ L.bad = 1; break; }
            st->tab_packed = b + r.off; st->tab_bits = r.bits;
            st->tab_norms = (const uint16_t *)(b + r.off + (size_t)r.shape[0] * n->eg_sub * r.bits / 8);
            if(!mat_alloc(&st->kv, 2 * D, n->eg_tables * n->eg_sub)){ L.bad = 1; break; }
            ld_cq_into(&L, &st->kv, 0, NULL, 0);
            ld_cq_into(&L, &st->kv, D, NULL, 0);
            st->taps = ld_f(&L, n->eg_ctaps * D);
        }
        n->final_norm = ld_f(&L, D);
        /* the probe heads: a manifest, then the confidence head (code 2) if present */
        if(!L.bad && L.idx < num_t - 1){
            float *man = ld_f(&L, 0);
            rec_t mr; read_rec(n, cb_n, L.idx - 1, &mr);
            int heads = mr.shape[0];
            for(int hi = 0; hi < heads && !L.bad; hi++){
                int code = man ? (int)(man[hi] + 0.5f) : 0;
                if(code == 2){
                    rec_t pr; read_rec(n, cb_n, L.idx, &pr);
                    int R = n->L + 1, K = pr.shape[0] / R, Qn;
                    n->n_probe = K;
                    n->probes = xalloc((size_t)R * K * D * 4);
                    for(int i = 0; i < R * K && n->probes; i++){
                        const uint8_t *pk = b + pr.off;
                        cq_row(n, pk, (const uint16_t *)(pk + (size_t)pr.shape[0] * D * pr.bits / 8), pr.bits, 128, D, D, i, n->probes + (size_t)i * D, n->tmp);
                    }
                    L.idx++;
                    n->gain = ld_f(&L, R * K);
                    rec_t qr; read_rec(n, cb_n, L.idx, &qr);
                    Qn = qr.shape[0]; n->n_query = Qn;
                    n->query = xalloc((size_t)Qn * D * 4);
                    for(int i = 0; i < Qn && n->query; i++){
                        const uint8_t *pk = b + qr.off;
                        cq_row(n, pk, (const uint16_t *)(pk + (size_t)qr.shape[0] * D * qr.bits / 8), qr.bits, 128, D, D, i, n->query + (size_t)i * D, n->tmp);
                    }
                    L.idx++;
                    n->row_bias = ld_f(&L, Qn * R * K);
                    rec_t jr; read_rec(n, cb_n, L.idx, &jr);
                    n->proj = xalloc((size_t)Qn * D * 4);
                    if(n->proj && jr.shape[0] == 1 && jr.shape[1] == Qn * D){
                        const uint8_t *pk = b + jr.off;
                        cq_row(n, pk, (const uint16_t *)(pk + (size_t)Qn * D * jr.bits / 8), jr.bits, 128, Qn * D, Qn * D, 0, n->proj, n->tmp);
                    }
                    L.idx++;
                    float *bb = ld_f(&L, 1); n->proj_bias = bb ? bb[0] : 0.f; xfree(bb);
                    n->has_head = n->probes && n->gain && n->query && n->row_bias && n->proj;
                } else {
                    L.idx += code == 3 ? 7 : 6;              /* another head: skip its tensors */
                }
            }
            xfree(man);
        }
        /* the tokenizer: the last tensor */
        rec_t tr;
        if(!L.bad && read_rec(n, cb_n, num_t - 1, &tr) && tr.dtype == 4){
            if(!load_tokenizer(n, b + tr.off, tr.nbytes)) L.bad = 1;
        } else L.bad = 1;
    }
    xfree(L.row); xfree(L.tmp);
    if(L.bad){ copy_err(err, err_len, "the archive is damaged or of another layout"); thim_needle_close(n); return NULL; }

    /* state and scratch */
    n->tokens = xalloc(MAXT * sizeof(int));
    n->K = xalloc((size_t)n->L * MAXT * KW * 4); n->V = xalloc((size_t)n->L * MAXT * VW * 4);
    int half = n->qk / 2;
    n->cosb = xalloc((size_t)MAXT * half * 4); n->sinb = xalloc((size_t)MAXT * half * 4);
    n->X = xalloc((size_t)CH * 4 * D * 4); n->x0 = xalloc((size_t)CH * D * 4); n->u = xalloc((size_t)CH * D * 4);
    n->hb = xalloc((size_t)CH * D * 4); n->nx = xalloc((size_t)CH * 4 * D * 4); n->mo = xalloc((size_t)CH * 24 * 4);
    n->raw = xalloc((size_t)CH * (n->qkv + D) * 4); n->qb = xalloc((size_t)QW * 4); n->kb = xalloc((size_t)KW * 4); n->vb = xalloc((size_t)VW * 4);
    n->att = xalloc((size_t)CH * D * 4); n->e = xalloc((size_t)CH * (D > n->eg_tables * n->eg_sub ? D : n->eg_tables * n->eg_sub) * 4);
    n->ekv = xalloc((size_t)CH * 2 * D * 4); n->ek = xalloc((size_t)(n->n_sites + 1) * CH * D * 4); n->ev = xalloc((size_t)(n->n_sites + 1) * CH * D * 4);
    n->sc = xalloc((size_t)MAXT * 4); n->z = xalloc((size_t)n->hada_n * 4); n->zt = xalloc((size_t)n->hada_n * 4);
    n->cond = xalloc((size_t)n->hada_n * 4); n->logits = xalloc((size_t)n->out_vocab * 4);
    if(!n->tokens || !n->K || !n->V || !n->cosb || !n->sinb || !n->X || !n->x0 || !n->u || !n->hb || !n->nx || !n->mo || !n->raw
       || !n->qb || !n->kb || !n->vb || !n->att || !n->e || !n->ekv || !n->ek || !n->ev || !n->sc || !n->z || !n->zt || !n->cond
       || !n->logits || !n->tmp || !snap_alloc(n, &n->cur) || !snap_alloc(n, &n->pre)){
        copy_err(err, err_len, "out of memory"); thim_needle_close(n); return NULL;
    }
    for(int t = 0; t < MAXT; t++)
        for(int i = 0; i < half; i++){
            /* freqs and angles in float32 like the reference, then sin/cos */
            float fr = 1.f / fexp(flog(n->rope_theta) * (float)(2 * i) / (float)n->qk);
            float ang = (float)t * fr;
            fsincos((double)ang, n->sinb + (size_t)t * half + i, n->cosb + (size_t)t * half + i);
        }
    snap_clear(n, &n->cur);
    return n;
}

void thim_needle_reset(thim_needle *n){ n->n = 0; snap_clear(n, &n->cur); }
int thim_needle_feed(thim_needle *n, const int *ids, int count, float *logits){
    if(feed(n, ids, count, 1) != 0) return -1;
    if(logits) memcpy(logits, n->logits, (size_t)n->out_vocab * 4);
    return 0;
}
float thim_needle_head(thim_needle *n){ return conf_head(n, &n->cur); }

/* ================= tools: a small JSON reader and the decode grammar ================= */
#define G_MAXTOOLS 16
#define G_MAXPROPS 12
#define G_MAXENUM  24
enum { T_STRING, T_INTEGER, T_NUMBER, T_BOOLEAN, T_ENUM };
typedef struct {
    char *name; int type;
    char *enums[G_MAXENUM]; int n_enum;
    int has_min, has_max, ex_min, ex_max; double mn, mx;
    int min_len, max_len;
} prop_t;
typedef struct { char *name; prop_t p[G_MAXPROPS]; int np; uint32_t required; } tool_t;
struct gram { tool_t t[G_MAXTOOLS]; int nt; };

static void gram_free(struct gram *g){
    if(!g) return;
    for(int i = 0; i < g->nt; i++){
        xfree(g->t[i].name);
        for(int k = 0; k < g->t[i].np; k++){ xfree(g->t[i].p[k].name); for(int e = 0; e < g->t[i].p[k].n_enum; e++) xfree(g->t[i].p[k].enums[e]); }
    }
    xfree(g);
}

/* JSON: a cursor over the text; values are skipped or read as needed */
typedef struct { const char *s; int i, n, bad; } js_t;
static void js_ws(js_t *j){ while(j->i < j->n && (j->s[j->i] == ' ' || j->s[j->i] == '\n' || j->s[j->i] == '\t' || j->s[j->i] == '\r')) j->i++; }
static int js_peek(js_t *j){ js_ws(j); return j->i < j->n ? j->s[j->i] : 0; }
static int js_eat(js_t *j, char c){ if(js_peek(j) == c){ j->i++; return 1; } return 0; }
static char *js_str(js_t *j){                          /* a string value, unescaped (\uXXXX kept as '?') */
    if(!js_eat(j, '"')){ j->bad = 1; return NULL; }
    int start = j->i, cap = 0;
    while(j->i < j->n && j->s[j->i] != '"'){ if(j->s[j->i] == '\\') j->i++; j->i++; cap++; }
    if(j->i >= j->n){ j->bad = 1; return NULL; }
    char *out = xalloc((size_t)cap + 1);
    if(!out){ j->bad = 1; return NULL; }
    int k = 0;
    for(int i = start; i < j->i; i++){
        char c = j->s[i];
        if(c == '\\' && i + 1 < j->i){
            char e = j->s[++i];
            if(e == 'n') c = '\n'; else if(e == 't') c = '\t'; else if(e == 'u'){ c = '?'; i += 4; } else c = e;
        }
        out[k++] = c;
    }
    out[k] = 0;
    j->i++;
    return out;
}
static double js_num(js_t *j){
    js_ws(j);
    double v = 0, sgn = 1, frac = 0.1; int any = 0;
    if(j->i < j->n && j->s[j->i] == '-'){ sgn = -1; j->i++; }
    while(j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9'){ v = v * 10 + (j->s[j->i] - '0'); j->i++; any = 1; }
    if(j->i < j->n && j->s[j->i] == '.'){ j->i++; while(j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9'){ v += frac * (j->s[j->i] - '0'); frac *= 0.1; j->i++; any = 1; } }
    if(j->i < j->n && (j->s[j->i] == 'e' || j->s[j->i] == 'E')){ j->i++; if(j->s[j->i] == '+' || j->s[j->i] == '-') j->i++; while(j->i < j->n && j->s[j->i] >= '0' && j->s[j->i] <= '9') j->i++; }
    if(!any) j->bad = 1;
    return v * sgn;
}
static void js_skip(js_t *j){
    int c = js_peek(j);
    if(c == '"'){ xfree(js_str(j)); return; }
    if(c == '{' || c == '['){
        char open = (char)c, close = c == '{' ? '}' : ']';
        j->i++;
        if(js_eat(j, close)) return;
        do {
            if(open == '{'){ xfree(js_str(j)); if(!js_eat(j, ':')){ j->bad = 1; return; } }
            js_skip(j);
            if(j->bad) return;
        } while(js_eat(j, ','));
        if(!js_eat(j, close)) j->bad = 1;
        return;
    }
    if(c == 't' || c == 'f' || c == 'n'){ while(j->i < j->n && j->s[j->i] >= 'a' && j->s[j->i] <= 'z') j->i++; return; }
    js_num(j);
}
static int streq(const char *a, const char *b){ return a && b && !strcmp(a, b); }

static int parse_prop(js_t *j, prop_t *p){
    if(!js_eat(j, '{')) return 0;
    p->type = T_STRING; p->max_len = 200;
    int tstr = 0;
    if(js_eat(j, '}')) return 1;
    do {
        char *k = js_str(j);
        if(!k || !js_eat(j, ':')){ xfree(k); return 0; }
        if(streq(k, "type")){
            char *t = js_str(j);
            if(streq(t, "integer")) p->type = T_INTEGER; else if(streq(t, "number")) p->type = T_NUMBER;
            else if(streq(t, "boolean")) p->type = T_BOOLEAN; else tstr = 1;
            xfree(t);
        } else if(streq(k, "enum")){
            if(!js_eat(j, '[')){ xfree(k); return 0; }
            if(!js_eat(j, ']')){
                do {
                    if(js_peek(j) == '"'){ char *e = js_str(j); if(p->n_enum < G_MAXENUM) p->enums[p->n_enum++] = e; else xfree(e); }
                    else js_skip(j);
                } while(js_eat(j, ','));
                if(!js_eat(j, ']')){ xfree(k); return 0; }
            }
        } else if(streq(k, "minimum")){ p->has_min = 1; p->mn = js_num(j); }
        else if(streq(k, "maximum")){ p->has_max = 1; p->mx = js_num(j); }
        else if(streq(k, "exclusiveMinimum")){ p->has_min = 1; p->ex_min = 1; p->mn = js_num(j); }
        else if(streq(k, "exclusiveMaximum")){ p->has_max = 1; p->ex_max = 1; p->mx = js_num(j); }
        else if(streq(k, "minLength")) p->min_len = (int)js_num(j);
        else if(streq(k, "maxLength")) p->max_len = (int)js_num(j);
        else js_skip(j);
        xfree(k);
        if(j->bad) return 0;
    } while(js_eat(j, ','));
    if(!js_eat(j, '}')) return 0;
    if(p->n_enum && (tstr || p->type == T_STRING)) p->type = T_ENUM;
    return 1;
}

static struct gram *gram_build(const char *tools_json){
    struct gram *g = xalloc(sizeof *g);
    if(!g) return NULL;
    js_t j = { tools_json, 0, (int)strlen(tools_json), 0 };
    if(!js_eat(&j, '[')) goto bad;
    if(js_eat(&j, ']')) return g;
    do {
        if(g->nt >= G_MAXTOOLS || !js_eat(&j, '{')) goto bad;
        tool_t *t = &g->t[g->nt++];
        char *req[G_MAXPROPS]; int nreq = 0;
        int empty_tool = js_eat(&j, '}');
        if(!empty_tool) do {
            char *k = js_str(&j);
            if(!k || !js_eat(&j, ':')){ xfree(k); goto bad; }
            if(streq(k, "name")) t->name = js_str(&j);
            else if(streq(k, "parameters")){
                if(!js_eat(&j, '{')){ xfree(k); goto bad; }
                int empty_params = js_eat(&j, '}');
                if(!empty_params) do {
                    char *pk = js_str(&j);
                    if(!pk || !js_eat(&j, ':')){ xfree(pk); xfree(k); goto bad; }
                    if(streq(pk, "properties")){
                        if(!js_eat(&j, '{')){ xfree(pk); xfree(k); goto bad; }
                        int empty_props = js_eat(&j, '}');
                        if(!empty_props) do {
                            if(t->np >= G_MAXPROPS){ xfree(pk); xfree(k); goto bad; }
                            prop_t *p = &t->p[t->np++];
                            p->name = js_str(&j);
                            if(!p->name || !js_eat(&j, ':') || !parse_prop(&j, p)){ xfree(pk); xfree(k); goto bad; }
                        } while(js_eat(&j, ','));
                        if(!empty_props && !js_eat(&j, '}')){ xfree(pk); xfree(k); goto bad; }
                    } else if(streq(pk, "required")){
                        if(!js_eat(&j, '[')){ xfree(pk); xfree(k); goto bad; }
                        if(!js_eat(&j, ']')){
                            do { char *r = js_str(&j); if(nreq < G_MAXPROPS) req[nreq++] = r; else xfree(r); } while(js_eat(&j, ','));
                            if(!js_eat(&j, ']')){ xfree(pk); xfree(k); goto bad; }
                        }
                    } else js_skip(&j);
                    xfree(pk);
                } while(js_eat(&j, ','));
                if(!empty_params && !js_eat(&j, '}')){ xfree(k); goto bad; }
            } else js_skip(&j);
            xfree(k);
            if(j.bad) goto bad;
        } while(js_eat(&j, ','));
        if((!empty_tool && !js_eat(&j, '}')) || !t->name) goto bad;
        for(int r = 0; r < nreq; r++){
            for(int k = 0; k < t->np; k++) if(streq(t->p[k].name, req[r])) t->required |= 1u << k;
            xfree(req[r]);
        }
    } while(js_eat(&j, ','));
    if(!js_eat(&j, ']')) goto bad;
    return g;
bad:
    gram_free(g);
    return NULL;
}

/* The decode grammar, byte by byte (markers are their own symbols):
 *   [<think> any text </think> \n*] <tool_call> [ CALL (, CALL)* | (nothing: refusal) ] </tool_call> <|im_end|>
 *   CALL = {"name":"NAME","arguments":{ ("KEY":VALUE)(,"KEY":VALUE)* }}
 * with NAME a declared tool, each KEY a property of it at most once, required ones present
 * before the object closes, and VALUE typed: strings without quotes, backslashes or control
 * bytes inside their length bounds, enum strings from the list, numbers inside their bounds. */
enum { G_START, G_THINK, G_AFTER_THINK, G_OPEN, G_LIST_FIRST, G_LIT, G_NAME, G_ARGS_FIRST, G_KEY, G_VALUE,
       G_STR, G_ENUM, G_NUM, G_AFTER_VALUE, G_ARGS_CLOSE, G_AFTER_CALL, G_LIST_END, G_DONE_CALL, G_FINISHED };
#define G_MAXCALLS 4
typedef struct {
    int ph, next, tool, prop, ncalls, nchars, nlen, ndot, nneg, nfrac, nlead0, think_tokens, no_refusal;
    uint32_t used;
    const char *lit; int lp;
    char buf[96]; int bl;
    double nval;
} gst_t;

static int set_prefix_ok(const struct gram *g, const gst_t *s, int kind){   /* kind 0 names, 1 keys, 2 enum */
    for(int i = 0; i < (kind == 0 ? g->nt : kind == 1 ? g->t[s->tool].np : g->t[s->tool].p[s->prop].n_enum); i++){
        const char *c = kind == 0 ? g->t[i].name : kind == 1 ? g->t[s->tool].p[i].name : g->t[s->tool].p[s->prop].enums[i];
        if(kind == 1 && (s->used >> i) & 1) continue;
        if((int)strlen(c) >= s->bl && !memcmp(c, s->buf, (size_t)s->bl)) return 1;
    }
    return 0;
}
static int set_exact(const struct gram *g, const gst_t *s, int kind){
    for(int i = 0; i < (kind == 0 ? g->nt : kind == 1 ? g->t[s->tool].np : g->t[s->tool].p[s->prop].n_enum); i++){
        const char *c = kind == 0 ? g->t[i].name : kind == 1 ? g->t[s->tool].p[i].name : g->t[s->tool].p[s->prop].enums[i];
        if(kind == 1 && (s->used >> i) & 1) continue;
        if((int)strlen(c) == s->bl && !memcmp(c, s->buf, (size_t)s->bl)) return i;
    }
    return -1;
}
static void g_lit(gst_t *s, const char *lit, int next){ s->ph = G_LIT; s->lit = lit; s->lp = 0; s->next = next; }

static int num_ok_final(const prop_t *p, const gst_t *s){
    if(s->nlen == 0 || (s->nneg && s->nlen == 1) || (s->ndot && s->nfrac == 0)) return 0;
    double v = s->nval;
    if(p->has_min && (p->ex_min ? v <= p->mn : v < p->mn)) return 0;
    if(p->has_max && (p->ex_max ? v >= p->mx : v > p->mx)) return 0;
    return 1;
}
static int all_required(const struct gram *g, const gst_t *s){ return (g->t[s->tool].required & ~s->used) == 0; }
static int any_unused(const struct gram *g, const gst_t *s){ return (s->used & ((1u << g->t[s->tool].np) - 1)) != ((1u << g->t[s->tool].np) - 1); }

static int g_byte(const struct gram *g, gst_t *s, uint8_t c){
    switch(s->ph){
    case G_THINK: return c != 0;
    case G_AFTER_THINK: return c == '\n';
    case G_OPEN: if(c == '['){ s->ph = G_LIST_FIRST; return 1; } return 0;
    case G_LIST_FIRST:
        if(c == ']' && !s->no_refusal && s->ncalls == 0){ s->ph = G_LIST_END; return 1; }
        if(c == '{' && s->ncalls < G_MAXCALLS){ s->bl = 0; g_lit(s, "\"name\":\"", G_NAME); return 1; }
        return 0;
    case G_LIT:
        if(c != (uint8_t)s->lit[s->lp]) return 0;
        if(!s->lit[++s->lp]) s->ph = s->next;
        return 1;
    case G_NAME:
        if(c == '"'){ int t = set_exact(g, s, 0); if(t < 0) return 0; s->tool = t; s->used = 0; g_lit(s, ",\"arguments\":{", G_ARGS_FIRST); return 1; }
        if(s->bl >= (int)sizeof s->buf - 1) return 0;
        s->buf[s->bl++] = (char)c;
        if(!set_prefix_ok(g, s, 0)){ s->bl--; return 0; }
        return 1;
    case G_ARGS_FIRST:
        if(c == '"' && any_unused(g, s)){ s->bl = 0; s->ph = G_KEY; return 1; }
        if(c == '}' && all_required(g, s)){ s->ph = G_ARGS_CLOSE; return 1; }
        return 0;
    case G_KEY:
        if(c == '"'){ int k = set_exact(g, s, 1); if(k < 0) return 0; s->prop = k; s->used |= 1u << k; g_lit(s, ":", G_VALUE); return 1; }
        if(s->bl >= (int)sizeof s->buf - 1) return 0;
        s->buf[s->bl++] = (char)c;
        if(!set_prefix_ok(g, s, 1)){ s->bl--; return 0; }
        return 1;
    case G_VALUE: {
        const prop_t *p = &g->t[s->tool].p[s->prop];
        if(p->type == T_STRING){ if(c != '"') return 0; s->nchars = 0; s->ph = G_STR; return 1; }
        if(p->type == T_ENUM){ if(c != '"') return 0; s->bl = 0; s->ph = G_ENUM; return 1; }
        if(p->type == T_BOOLEAN){
            if(c == 't'){ g_lit(s, "rue", G_AFTER_VALUE); return 1; }
            if(c == 'f'){ g_lit(s, "alse", G_AFTER_VALUE); return 1; }
            return 0;
        }
        s->nlen = s->ndot = s->nneg = s->nfrac = s->nlead0 = 0; s->nval = 0;
        s->ph = G_NUM;
        return g_byte(g, s, c);
    }
    case G_STR: {
        const prop_t *p = &g->t[s->tool].p[s->prop];
        if(c == '"'){ if(s->nchars < p->min_len) return 0; s->ph = G_AFTER_VALUE; return 1; }
        if(c == '\\' || c < 0x20) return 0;
        if((c & 0xC0) != 0x80){ if(s->nchars >= p->max_len) return 0; s->nchars++; }
        return 1;
    }
    case G_ENUM:
        if(c == '"'){ if(set_exact(g, s, 2) < 0) return 0; s->ph = G_AFTER_VALUE; return 1; }
        if(s->bl >= (int)sizeof s->buf - 1) return 0;
        s->buf[s->bl++] = (char)c;
        if(!set_prefix_ok(g, s, 2)){ s->bl--; return 0; }
        return 1;
    case G_NUM: {
        const prop_t *p = &g->t[s->tool].p[s->prop];
        if(c == ',' || c == '}'){
            if(!num_ok_final(p, s)) return 0;
            s->ph = G_AFTER_VALUE;
            return g_byte(g, s, c);
        }
        if(s->nlen >= 20) return 0;
        if(c == '-'){
            if(s->nlen || (p->has_min && p->mn >= 0)) return 0;
            s->nneg = 1; s->nlen++; return 1;
        }
        if(c == '.'){
            if(p->type != T_NUMBER || s->ndot || s->nlen == s->nneg) return 0;
            s->ndot = 1; s->nlen++; return 1;
        }
        if(c < '0' || c > '9') return 0;
        int intdigits = s->nlen - s->nneg - s->ndot - s->nfrac;
        if(!s->ndot && s->nlead0) return 0;                   /* "0" takes no more integer digits */
        double nv;
        if(s->ndot){ double f = 1; for(int i = 0; i <= s->nfrac; i++) f *= 0.1; nv = s->nval + (s->nneg ? -1 : 1) * (c - '0') * f; s->nfrac++; }
        else { nv = s->nval * 10 + (s->nneg ? -1 : 1) * (c - '0'); if(intdigits == 0 && c == '0') s->nlead0 = 1; }
        /* more digits only move away from zero: a value already past a bound cannot come back */
        if(!s->ndot){
            if(!s->nneg && p->has_max && nv > p->mx) return 0;
            if(s->nneg && p->has_min && nv < p->mn) return 0;
        }
        s->nval = nv; s->nlen++;
        return 1;
    }
    case G_AFTER_VALUE:
        if(c == ','){ if(!any_unused(g, s)) return 0; g_lit(s, "\"", G_KEY); s->bl = 0; return 1; }
        if(c == '}'){ if(!all_required(g, s)) return 0; s->ph = G_ARGS_CLOSE; return 1; }
        return 0;
    case G_ARGS_CLOSE: if(c == '}'){ s->ph = G_AFTER_CALL; s->ncalls++; return 1; } return 0;
    case G_AFTER_CALL:
        if(c == ']'){ s->ph = G_LIST_END; return 1; }
        if(c == ',' && s->ncalls < G_MAXCALLS){ s->ph = G_LIST_FIRST; return 1; }
        return 0;
    default: return 0;
    }
}

/* a whole token: 1 if the grammar takes it from state s (s is advanced) */
static int g_token(const thim_needle *n, const struct gram *g, gst_t *s, int id, int max_think){
    char bytes[64]; int marker;
    int k = token_bytes(n, id, bytes, &marker);
    if(marker){
        if(id == n->tk_think && s->ph == G_START){ s->ph = G_THINK; s->think_tokens = 0; return 1; }
        if(id == n->tk_think_end && s->ph == G_THINK){ s->ph = G_AFTER_THINK; return 1; }
        if(id == n->tk_call && (s->ph == G_START || s->ph == G_AFTER_THINK)){ s->ph = G_OPEN; return 1; }
        if(id == n->tk_call_end && s->ph == G_LIST_END){ s->ph = G_DONE_CALL; return 1; }
        if(id == n->tk_im_end && s->ph == G_DONE_CALL){ s->ph = G_FINISHED; return 1; }
        return 0;
    }
    if(k == 0) return 0;
    if(s->ph == G_THINK){
        if(s->think_tokens >= max_think) return 0;
        for(int i = 0; i < k; i++) if(bytes[i] == 0) return 0;
        s->think_tokens++;
        return 1;
    }
    for(int i = 0; i < k; i++) if(!g_byte(g, s, (uint8_t)bytes[i])) return 0;
    return 1;
}

/* ---------------- grounding gates: is the call evidenced by the request? ---------------- */
#define GW 48
static int lower_words(const char *s, char w[][24], int max){
    int n = 0;
    while(*s && n < max){
        while(*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '\'' || (uint8_t)*s >= 0x80)) s++;
        if(!*s) break;
        int k = 0;
        while(*s && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '\'' || (uint8_t)*s >= 0x80)){
            char c = *s++;
            if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if(k < 23) w[n][k++] = c;
        }
        while(k && w[n][k - 1] == '\'') k--;
        w[n][k] = 0;
        if(k) n++;
    }
    return n;
}
static int word_in(const char *w, char q[][24], int nq){
    int lw = (int)strlen(w);
    for(int i = 0; i < nq; i++){
        if(!strcmp(w, q[i])) return 1;
        int lq = (int)strlen(q[i]);                       /* plural / singular: task ~ tasks */
        if(lw == lq + 1 && w[lw - 1] == 's' && !strncmp(w, q[i], (size_t)lq)) return 1;
        if(lq == lw + 1 && q[i][lq - 1] == 's' && !strncmp(w, q[i], (size_t)lw)) return 1;
    }
    return 0;
}
static int has_negation(char q[][24], int nq, const char *tools_lc){
    for(int i = 0; i < nq; i++){
        const char *w = q[i];
        if(tools_lc && i > 0 && i + 1 < nq){                /* a name the tools use, like "do not disturb" */
            char ph[80]; int k = 0;
            for(int j = i - 1; j <= i + 1; j++){ for(const char *c = q[j]; *c && k < 76; c++) ph[k++] = *c; if(j < i + 1) ph[k++] = ' '; }
            ph[k] = 0;
            if(strstr(tools_lc, ph)) continue;
        }
        if(!strcmp(w, "don't") || !strcmp(w, "dont") || !strcmp(w, "never") || !strcmp(w, "not") || !strcmp(w, "doesn't")
           || !strcmp(w, "shouldn't") || !strcmp(w, "won't")) return 1;
    }
    return 0;
}
/* the numbers a request states: digit runs, number words (forty five, a hundred), and the few
 * words that mean an end of a scale (mute 0, half 50, max / full 100) */
static int request_numbers(const char *query, char q[][24], int nq, double *out, int max){
    int n = 0;
    for(const char *p = query; *p && n < max; ){
        if(*p >= '0' && *p <= '9'){
            double v = 0; while(*p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; }
            if(*p == '.' && p[1] >= '0' && p[1] <= '9'){ double f = 0.1; p++; while(*p >= '0' && *p <= '9'){ v += f * (*p - '0'); f *= 0.1; p++; } }
            out[n++] = v;
        } else p++;
    }
    static const char *ones[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
                                  "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
    static const char *tens[] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };
    for(int i = 0; i < nq && n < max; i++){
        int v = -1;
        for(int k = 0; k < 20; k++) if(!strcmp(q[i], ones[k])) v = k;
        for(int k = 2; k < 10; k++){
            int lt = (int)strlen(tens[k]);
            if(!strncmp(q[i], tens[k], (size_t)lt)){
                v = k * 10;
                if(q[i][lt] == '-' ) for(int u = 1; u < 10; u++) if(!strcmp(q[i] + lt + 1, ones[u])) v += u;
                if(!q[i][lt] && i + 1 < nq) for(int u = 1; u < 10; u++) if(!strcmp(q[i + 1], ones[u])){ out[n++] = v; v += u; }
            }
        }
        if(!strcmp(q[i], "hundred")){ v = 100; if(i > 0) for(int u = 1; u < 10; u++) if(!strcmp(q[i - 1], ones[u])) v = u * 100; }
        if(!strcmp(q[i], "mute") || !strcmp(q[i], "muted") || !strcmp(q[i], "silent")) v = 0;
        if(!strcmp(q[i], "half")) v = 50;
        if(!strcmp(q[i], "max") || !strcmp(q[i], "maximum") || !strcmp(q[i], "full")) v = 100;
        if(v >= 0 && n < max) out[n++] = v;
    }
    return n;
}
static const prop_t *find_prop(const struct gram *g, const char *tool, const char *key){
    for(int i = 0; i < g->nt; i++) if(!strcmp(g->t[i].name, tool))
        for(int k = 0; k < g->t[i].np; k++) if(!strcmp(g->t[i].p[k].name, key)) return &g->t[i].p[k];
    return NULL;
}
/* calls is grammar-made: [{"name":"N","arguments":{"k":v,...}},...] with plain strings */
static int gate_calls(const struct gram *g, const char *query, char *calls, int gates, const char *tools_lc){
    static char q[GW][24];
    int nq = lower_words(query, q, GW);
    if((gates & THIM_GATE_NEGATION) && has_negation(q, nq, tools_lc)) return THIM_WITHHELD_NEGATION;
    double nums[32]; int nn = request_numbers(query, q, nq, nums, 32);
    char *p = calls;
    char tool[64] = "";
    while((p = strchr(p, '"'))){
        char *kq = p;
        const char *k0 = ++p; char *k1 = strchr(p, '"');
        if(!k1) break;
        int kl = (int)(k1 - k0);
        p = k1 + 1;
        if(*p != ':') continue;
        p++;
        if(kl == 4 && !strncmp(k0, "name", 4) && *p == '"'){
            char *e = strchr(p + 1, '"'); if(!e) break;
            int l = (int)(e - p - 1); if(l > 63) l = 63;
            memcpy(tool, p + 1, (size_t)l); tool[l] = 0; p = e + 1; continue;
        }
        if(kl == 9 && !strncmp(k0, "arguments", 9)) continue;
        char key[64]; int l = kl > 63 ? 63 : kl; memcpy(key, k0, (size_t)l); key[l] = 0;
        const prop_t *pr = find_prop(g, tool, key);
        if(!pr) continue;
        if((pr->type == T_INTEGER || pr->type == T_NUMBER) && (gates & THIM_GATE_NUMBERS)){
            double v = 0, sg = 1, f = 0.1; int dot = 0;
            if(*p == '-'){ sg = -1; p++; }
            while((*p >= '0' && *p <= '9') || *p == '.'){ if(*p == '.') dot = 1; else if(dot){ v += f * (*p - '0'); f *= 0.1; } else v = v * 10 + (*p - '0'); p++; }
            v *= sg;
            int found = 0;
            for(int i = 0; i < nn; i++){ double d = nums[i] - v; if(d < 1e-9 && d > -1e-9) found = 1; }
            if(!found) return THIM_WITHHELD_NUMBER;
        } else if(pr->type == T_ENUM && *p == '"' && (gates & THIM_GATE_ENUM)){
            char *e = strchr(p + 1, '"'); if(!e) break;
            char val[96]; int vl = (int)(e - p - 1); if(vl > 95) vl = 95;
            memcpy(val, p + 1, (size_t)vl); val[vl] = 0;
            static char vw[GW][24];
            int nv = lower_words(val, vw, GW), named = 1;
            for(int i = 0; i < nv; i++) if(!word_in(vw[i], q, nq)) named = 0;
            if(!named){
                int required = 0;
                for(int i = 0; i < g->nt; i++) if(!strcmp(g->t[i].name, tool))
                    for(int k = 0; k < g->t[i].np; k++) if(g->t[i].p[k] .name && !strcmp(g->t[i].p[k].name, key)) required = (g->t[i].required >> k) & 1;
                if(required) return THIM_WITHHELD_ENUM;
                /* optional: drop "key":"value" and one comma next to it */
                char *a = kq, *b = e + 1;
                if(*b == ',') b++; else if(a > calls && a[-1] == ',') a--;
                memmove(a, b, strlen(b) + 1);
                p = a;
                continue;
            }
            p = e + 1;
        } else if(pr->type == T_STRING && *p == '"' && (gates & THIM_GATE_TEXT)){
            const char *e = strchr(p + 1, '"'); if(!e) break;
            char val[256]; int vl = (int)(e - p - 1); if(vl > 255) vl = 255;
            memcpy(val, p + 1, (size_t)vl); val[vl] = 0;
            static char vw[GW][24];
            int nv = lower_words(val, vw, GW);
            for(int i = 0; i < nv; i++) if(!word_in(vw[i], q, nq)) return THIM_WITHHELD_TEXT;
            p = (char *)e + 1;
        }
    }
    return THIM_OK;
}

int thim_needle_set_tools(thim_needle *n, const char *tools_json, const char *system, char *err, size_t err_len){
    struct gram *g = gram_build(tools_json);
    if(!g){ copy_err(err, err_len, "the tools are not valid JSON schemas"); return -1; }
    gram_free(n->g); n->g = g;
    xfree(n->tools_json); xfree(n->system);
    size_t tl = strlen(tools_json), sl = system ? strlen(system) : 0;
    n->tools_json = xalloc(tl + 1); n->system = xalloc(sl + 1);
    if(!n->tools_json || !n->system){ copy_err(err, err_len, "out of memory"); return -1; }
    memcpy(n->tools_json, tools_json, tl); if(sl) memcpy(n->system, system, sl);
    xfree(n->tools_lc); n->tools_lc = xalloc(tl + 1);
    if(n->tools_lc) for(size_t i = 0; i < tl; i++){ char c = tools_json[i]; n->tools_lc[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    /* <BOS>[<|im_start|>system\nSYSTEM<|im_end|>\n]<|im_start|>user\n<tools>TOOLS</tools> */
    size_t cap = tl + sl + 128;
    char *text = xalloc(cap);
    int *ids = xalloc((MAXT + 8) * sizeof(int));
    if(!text || !ids){ xfree(text); xfree(ids); copy_err(err, err_len, "out of memory"); return -1; }
    size_t k = 0;
    #define CAT(s) do { const char *_s = (s); size_t _l = strlen(_s); memcpy(text + k, _s, _l); k += _l; } while(0)
    if(sl){ CAT("<|im_start|>system\n"); CAT(system); CAT("<|im_end|>\n"); }
    CAT("<|im_start|>user\n<tools>"); CAT(tools_json); CAT("</tools>");
    #undef CAT
    text[k] = 0;
    ids[0] = n->bos;
    int cnt = thim_needle_encode(n, text, ids + 1, MAXT);
    xfree(text);
    if(cnt < 0 || cnt + 1 > MAXT - 256){ xfree(ids); copy_err(err, err_len, "the tools take too much of the context"); return -1; }
    thim_needle_reset(n);
    feed(n, ids, cnt + 1, 0);
    xfree(ids);
    snap_copy(n, &n->pre, &n->cur);
    n->prefix_ready = 1;
    return 0;
}

int thim_needle_run(thim_needle *n, const char *query, const thim_needle_opts *opts, char *calls, size_t calls_cap,
                    char *reasoning, size_t reasoning_cap, thim_needle_result *res){
    thim_needle_result rr; memset(&rr, 0, sizeof rr);
    if(calls && calls_cap) calls[0] = 0;
    if(reasoning && reasoning_cap) reasoning[0] = 0;
    if(!n->prefix_ready || !n->g){ copy_err(calls, calls_cap, "no tools declared"); return -1; }
    int max_new = opts && opts->max_new_tokens > 0 ? opts->max_new_tokens : 160;
    int max_think = opts && opts->max_think_tokens > 0 ? opts->max_think_tokens : 64;
    int saved_window = n->window;
    if(opts && opts->local_window > 0) n->window = opts->local_window;
    /* back to the end of the tools */
    n->n = n->pre.len;
    snap_copy(n, &n->cur, &n->pre);
    size_t ql = strlen(query);
    char *text = xalloc(ql + 64);
    int *ids = xalloc((MAXT + 8) * sizeof(int));
    if(!text || !ids){ xfree(text); xfree(ids); n->window = saved_window; copy_err(calls, calls_cap, "out of memory"); return -1; }
    size_t k = 0;
    text[k++] = '\n'; memcpy(text + k, query, ql); k += ql;
    memcpy(text + k, "<|im_end|>\n<|im_start|>assistant\n", 33); k += 33; text[k] = 0;
    int cnt = thim_needle_encode(n, text, ids, MAXT);
    xfree(text);
    if(cnt < 0 || n->n + cnt + max_new >= MAXT){ xfree(ids); n->window = saved_window; copy_err(calls, calls_cap, "the request is too long"); return -1; }
    double t0 = now();
    feed(n, ids, cnt, 1);
    rr.prompt_tokens = n->n;
    double t1 = now();
    gst_t gs; memset(&gs, 0, sizeof gs);
    gs.no_refusal = opts ? opts->no_refusal : 0;
    float *lg = n->logits;
    int V = n->out_vocab, done = 0;
    double logp_call = 0.0; int in_call = 0;
    size_t cl = 0, rl = 0;
    for(int step = 0; step < max_new && !done; step++){
        /* the best token the grammar takes: try in order of logit */
        float mx = -1e30f;
        for(int i = 0; i < V; i++) if(lg[i] > mx) mx = lg[i];
        float sum = 0.f;
        for(int i = 0; i < V; i++) sum += fexp(lg[i] - mx);
        int pick = -1; gst_t next = gs;
        for(int tries = 0; tries < V; tries++){
            int best = -1; float bv = -1e30f;
            for(int i = 0; i < V; i++) if(lg[i] > bv){ bv = lg[i]; best = i; }
            if(best < 0 || bv <= -1e29f) break;
            gst_t tryst = gs;
            if(g_token(n, n->g, &tryst, best, max_think)){ pick = best; next = tryst; break; }
            lg[best] = -1e30f;                          /* not allowed: look at the next */
        }
        if(pick < 0) break;
        float p = fexp(lg[pick] - mx) / sum;
        if(pick == n->tk_call) in_call = 1;
        if(in_call && pick != n->tk_call) logp_call += (double)flog(p > 1e-30f ? p : 1e-30f);
        /* text out */
        char bytes[64]; int marker, nb = token_bytes(n, pick, bytes, &marker);
        if(!marker){
            if(gs.ph == G_THINK){ for(int i = 0; i < nb && reasoning && rl + 1 < reasoning_cap; i++) reasoning[rl++] = bytes[i]; }
            else if(in_call){ for(int i = 0; i < nb && calls && cl + 1 < calls_cap; i++) calls[cl++] = bytes[i]; }
        }
        gs = next;
        rr.new_tokens++;
        if(feed(n, &pick, 1, gs.ph != G_FINISHED) != 0) break;
        if(gs.ph == G_FINISHED) done = 1;
        else if(gs.ph == G_THINK && gs.think_tokens >= max_think){
            /* the reasoning ran long: close it ourselves */
            int close = n->tk_think_end;
            gs.ph = G_AFTER_THINK;
            if(feed(n, &close, 1, 1) != 0) break;
            rr.new_tokens++;
        }
    }
    xfree(ids);
    if(calls && cl < calls_cap) calls[cl] = 0;
    if(reasoning && rl < reasoning_cap){ while(rl && (reasoning[rl - 1] == '\n' || reasoning[rl - 1] == ' ')) rl--; reasoning[rl] = 0; }
    if(reasoning){ size_t a = 0; while(reasoning[a] == '\n') a++; if(a) memmove(reasoning, reasoning + a, strlen(reasoning + a) + 1); }
    double t2 = now();
    n->window = saved_window;
    if(!done){ copy_err(calls, calls_cap, "[]"); rr.n_calls = 0; }
    else rr.n_calls = gs.ncalls;
    rr.head = conf_head(n, &n->cur);
    rr.decode = (float)(logp_call < -80 ? 0.0 : (double)fexp((float)logp_call));
    rr.confidence = rr.head < 0 ? rr.decode : (rr.head < rr.decode ? rr.head : rr.decode);
    if(done && rr.n_calls > 0){
        float fl = opts && opts->floor > 0.f ? opts->floor : 0.1f;
        int gw = opts ? gate_calls(n->g, query, calls, opts->gates, n->tools_lc) : THIM_OK;
        rr.withheld = gw != THIM_OK ? gw : rr.confidence < fl ? THIM_WITHHELD_CONFIDENCE : THIM_OK;
    }
    rr.prefill_us = t1 - t0; rr.decode_us = t2 - t1;
    if(res) *res = rr;
    return done ? 0 : -1;
}
