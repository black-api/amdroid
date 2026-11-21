/*
 * Speech to text: a Whisper-style encoder/decoder run straight from a .thim file.
 *
 * Speed comes from what the file already holds (see tools/thim_convert.py): int8 weights in
 * 32-wide blocks, rows contiguous, convolutions as plain matrices, mel filters, window and
 * token bytes inside - so loading is pointer arithmetic. At run time:
 *   - the encoder only looks at as many frames as the audio needs (50 a second) instead of
 *     always a whole 30 s window, which is most of the work saved for a spoken sentence;
 *   - every matrix product quantizes its activations to int8 once and runs integer dot
 *     products over 32-wide blocks (the loops are written for the compiler's vectorizer; with
 *     SSE4.1 / AVX2 builds they use the multiply-add instructions);
 *   - the decoder keeps a key/value cache, and the encoder's keys/values for the cross attention
 *     are worked out once per utterance;
 *   - nothing calls libm: exp, log, erf and sqrt are small and local.
 */
#include "../include/thimble_stt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

#define BLOCK 32
#define MAGIC 0x4D494854u        /* "THIM" little endian */

/* ---------------- allocator and clock hooks ---------------- */
static void *(*g_alloc)(size_t) = malloc;
static void (*g_free)(void *) = free;
static double (*g_clock)(void) = NULL;
void thim_stt_set_alloc(void *(*a)(size_t), void (*r)(void *)){ g_alloc = a ? a : malloc; g_free = r ? r : free; }
void thim_stt_set_clock(double (*c)(void)){ g_clock = c; }
static void *xalloc(size_t n){ void *p = g_alloc(n + 64); if(!p) return NULL; memset(p, 0, n + 64); return p; }

/* ---------------- small math ---------------- */
static inline float fsqrt(float x){ return __builtin_sqrtf(x); }

static float fexp(float x){
    if(x < -87.f) return 0.f;
    if(x > 88.f) x = 88.f;
    float t = x * 1.44269504089f;                       /* x / ln 2 */
    int n = (int)(t < 0 ? t - 1 : t);                   /* floor */
    float f = (t - (float)n) * 0.69314718056f;          /* the remainder, in 0 .. ln 2 */
    /* e^f on [0, ln 2): degree 6 Taylor, error under 1e-7 */
    float p = 1.f + f * (1.f + f * (0.5f + f * (1.f / 6 + f * (1.f / 24 + f * (1.f / 120 + f * (1.f / 720))))));
    union { float f; uint32_t u; } v; v.u = (uint32_t)(n + 127) << 23;
    return p * v.f;
}

static float flog2(float x){                            /* x > 0 */
    union { float f; uint32_t u; } v; v.f = x;
    int e = (int)((v.u >> 23) & 255) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;            /* mantissa in [1, 2) */
    float m = v.f;
    /* log2(m) via atanh series on s = (m-1)/(m+1): precise to ~1e-7 over [1,2) */
    float s = (m - 1.f) / (m + 1.f), s2 = s * s;
    float l = s * (2.88539008f + s2 * (0.96179669f + s2 * (0.57707801f + s2 * (0.41219858f + s2 * 0.32059890f))));
    return (float)e + l;
}

static float ferf(float x){                             /* Abramowitz & Stegun 7.1.26, |err| < 1.5e-7 */
    float a = x < 0 ? -x : x;
    float t = 1.f / (1.f + 0.3275911f * a);
    float y = 1.f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t * fexp(-a * a);
    return x < 0 ? -y : y;
}
static inline float gelu(float x){ return 0.5f * x * (1.f + ferf(x * 0.70710678f)); }

/* ---------------- vector forms (AVX2; the other builds loop over the scalar ones) ---------------- */
#if defined(__AVX2__)
static inline __m256 vexp8(__m256 x){
    x = _mm256_max_ps(x, _mm256_set1_ps(-87.f)); x = _mm256_min_ps(x, _mm256_set1_ps(88.f));
    __m256 t = _mm256_mul_ps(x, _mm256_set1_ps(1.44269504089f));
    __m256 n = _mm256_floor_ps(t);
    __m256 f = _mm256_mul_ps(_mm256_sub_ps(t, n), _mm256_set1_ps(0.69314718056f));
    __m256 p = _mm256_set1_ps(1.f / 720);
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.f / 120)); p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.f / 24));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.f / 6)); p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.5f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.f)); p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.f));
    __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(n), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(p, _mm256_castsi256_ps(e));
}
static inline __m256 vgelu8(__m256 x){
    __m256 sx = _mm256_mul_ps(x, _mm256_set1_ps(0.70710678f));
    __m256 sign = _mm256_and_ps(sx, _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000)));
    __m256 a = _mm256_andnot_ps(_mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000)), sx);
    __m256 t = _mm256_div_ps(_mm256_set1_ps(1.f), _mm256_fmadd_ps(_mm256_set1_ps(0.3275911f), a, _mm256_set1_ps(1.f)));
    __m256 poly = _mm256_set1_ps(1.061405429f);
    poly = _mm256_fmadd_ps(poly, t, _mm256_set1_ps(-1.453152027f)); poly = _mm256_fmadd_ps(poly, t, _mm256_set1_ps(1.421413741f));
    poly = _mm256_fmadd_ps(poly, t, _mm256_set1_ps(-0.284496736f)); poly = _mm256_fmadd_ps(poly, t, _mm256_set1_ps(0.254829592f));
    __m256 y = _mm256_sub_ps(_mm256_set1_ps(1.f), _mm256_mul_ps(_mm256_mul_ps(poly, t), vexp8(_mm256_sub_ps(_mm256_setzero_ps(), _mm256_mul_ps(a, a)))));
    __m256 erf = _mm256_or_ps(y, sign);
    return _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(0.5f), x), _mm256_add_ps(_mm256_set1_ps(1.f), erf));
}
#endif

static void gelu_inplace(float *v, size_t n){
    size_t i = 0;
#if defined(__AVX2__)
    for(; i + 8 <= n; i += 8) _mm256_storeu_ps(v + i, vgelu8(_mm256_loadu_ps(v + i)));
#endif
    for(; i < n; i++) v[i] = gelu(v[i]);
}

/* sc[j] = exp(sc[j] - mx) for j < n; returns the sum */
static float softmax_exp(float *sc, int n, float mx){
    float sum = 0.f; int j = 0;
#if defined(__AVX2__)
    __m256 acc = _mm256_setzero_ps(), m = _mm256_set1_ps(mx);
    for(; j + 8 <= n; j += 8){ __m256 e = vexp8(_mm256_sub_ps(_mm256_loadu_ps(sc + j), m)); _mm256_storeu_ps(sc + j, e); acc = _mm256_add_ps(acc, e); }
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h)); h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1)); sum = _mm_cvtss_f32(h);
#endif
    for(; j < n; j++){ float e = fexp(sc[j] - mx); sc[j] = e; sum += e; }
    return sum;
}

static void cat_name(char *dst, size_t cap, const char *pre, const char *post){
    size_t k = 0;
    for(const char *q = pre; *q && k + 1 < cap; q++) dst[k++] = *q;
    for(const char *q = post; *q && k + 1 < cap; q++) dst[k++] = *q;
    dst[k] = 0;
}
/* ---------------- the model ---------------- */
typedef struct { uint32_t kind, rows, cols, cp; uint64_t data, scale; char name[64]; } entry_t;

typedef struct { uint32_t rows, cols, cp; const int8_t *q; const float *sc; } mat_t;
typedef struct { const float *w, *b; } ln_t;
typedef struct { mat_t q, k, v, o; const float *qb, *vb, *ob; } attn_t;
typedef struct { ln_t ln1, lnx, ln2; attn_t sa, xa; mat_t fc1, fc2; const float *b1, *b2; } layer_t;

struct thim_stt {
    const uint8_t *base; size_t len;
    int d, nh, ne, nd, ffn, nm, vocab, n_audio, n_text, n_fft, hop, rate, sot, eot, notimestamps;
    int nsup; const int32_t *sup; const uint32_t *toff; const uint8_t *tblob;
    const float *mel, *window, *enc_pos, *dec_pos;
    mat_t conv1, conv2, emb;
    const float *conv1b, *conv2b;
    ln_t enc_ln, dec_ln;
    layer_t enc[8], dec[8];
    /* tables built at open */
    float *dft_c, *dft_s;                               /* [201][400] */
    unsigned char *suppress;                            /* [vocab] */
    /* scratch, grown on demand */
    size_t cap_T;
    float *mel_buf, *x, *h, *q, *k, *v, *ctx, *f, *xk[8], *xv[8], *kc[8], *vc[8];
    int8_t *qa; float *qs; size_t qa_cap;
    float *scores;
    thim_stt_stats st;
};

/* header layout, matching thim_convert.py (little endian, no padding): "4sIII" + 17 x u32 + 6 x u64 */
static const uint8_t *hdr64_base(const thim_stt *s){ return s->base + 16 + 17 * 4; }
static uint64_t h64(const thim_stt *s, int i){ uint64_t v; memcpy(&v, hdr64_base(s) + 8 * i, 8); return v; }
static uint32_t h32(const thim_stt *s, int i){ uint32_t v; memcpy(&v, s->base + 16 + 4 * i, 4); return v; }

static const entry_t *table_find(const thim_stt *s, int n, const uint8_t *tab, const char *name){
    for(int i = 0; i < n; i++){
        const entry_t *e = (const entry_t *)(tab + 96 * i);
        if(!strcmp(e->name, name)) return e;
    }
    return NULL;
}

typedef struct { const thim_stt *s; int n; const uint8_t *tab; int bad; const char *missing; } loader_t;

static const float *L_f32(loader_t *L, const char *name){
    const entry_t *e = table_find(L->s, L->n, L->tab, name);
    if(!e || e->kind != 0){ L->bad = 1; L->missing = name; return NULL; }
    return (const float *)(L->s->base + e->data);
}
static mat_t L_q8(loader_t *L, const char *name){
    mat_t m; memset(&m, 0, sizeof m);
    const entry_t *e = table_find(L->s, L->n, L->tab, name);
    if(!e || e->kind != 1){ L->bad = 1; L->missing = name; return m; }
    m.rows = e->rows; m.cols = e->cols; m.cp = e->cp;
    m.q = (const int8_t *)(L->s->base + e->data); m.sc = (const float *)(L->s->base + e->scale);
    return m;
}
static ln_t L_ln(loader_t *L, const char *pre){
    char n[80]; ln_t l;
    cat_name(n, sizeof n, pre, ".w"); l.w = L_f32(L, n);
    cat_name(n, sizeof n, pre, ".b"); l.b = L_f32(L, n);
    return l;
}
static attn_t L_attn(loader_t *L, const char *pre){
    char n[80]; attn_t a;
    cat_name(n, sizeof n, pre, ".q.w"); a.q = L_q8(L, n);
    cat_name(n, sizeof n, pre, ".q.b"); a.qb = L_f32(L, n);
    cat_name(n, sizeof n, pre, ".k.w"); a.k = L_q8(L, n);
    cat_name(n, sizeof n, pre, ".v.w"); a.v = L_q8(L, n);
    cat_name(n, sizeof n, pre, ".v.b"); a.vb = L_f32(L, n);
    cat_name(n, sizeof n, pre, ".o.w"); a.o = L_q8(L, n);
    cat_name(n, sizeof n, pre, ".o.b"); a.ob = L_f32(L, n);
    return a;
}

static void tiny_trig(float *c, float *s_, int n_per_turn){      /* c[m], s_[m] = cos, sin of 2 pi m / n */
    const double PI2 = 6.283185307179586;
    int q4 = n_per_turn / 4;
    for(int m = 0; m < n_per_turn; m++){
        int q = m / q4, r = m % q4;
        double th = PI2 * r / n_per_turn, t2 = th * th, term = 1, cs = 1, sn = th, tt = th;
        for(int i = 1; i < 14; i++){ term *= -t2 / ((2 * i - 1) * (2 * i)); cs += term; }
        tt = th; sn = th;
        for(int i = 1; i < 14; i++){ tt *= -t2 / ((2 * i) * (2 * i + 1)); sn += tt; }
        double c0 = cs, s0 = sn;
        double cc = q == 0 ? c0 : q == 1 ? -s0 : q == 2 ? -c0 : s0;
        double ss = q == 0 ? s0 : q == 1 ? c0 : q == 2 ? -s0 : -c0;
        c[m] = (float)cc; s_[m] = (float)ss;
    }
}

/* name builder (no printf in the OS build): dst = pre + decimal(idx) + post */
static void mkname(char *dst, size_t cap, const char *pre, int idx, const char *post){
    size_t k = 0;
    for(const char *q = pre; *q && k + 1 < cap; q++) dst[k++] = *q;
    char d[12]; int n = 0; unsigned v = (unsigned)idx;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while(v && n < 11);
    while(n && k + 1 < cap) dst[k++] = d[--n];
    for(const char *q = post; *q && k + 1 < cap; q++) dst[k++] = *q;
    dst[k] = 0;
}
static void copy_err(char *err, size_t cap, const char *msg){
    if(!err || !cap) return;
    size_t k = 0; while(msg[k] && k + 1 < cap){ err[k] = msg[k]; k++; } err[k] = 0;
}

thim_stt *thim_stt_open(const void *thim, size_t len, char *err, size_t err_len){
    #define FAIL(msg) do { copy_err(err, err_len, msg); thim_stt_close(s); return NULL; } while(0)
    thim_stt *s = xalloc(sizeof *s);
    if(!s) return NULL;
    s->base = (const uint8_t *)thim; s->len = len;
    if(len < 512 || ((uintptr_t)thim & 7)) FAIL("not a .thim file (or not 8-byte aligned)");
    if(memcmp(s->base, "THIM", 4) != 0) FAIL("bad magic");
    uint32_t ver; memcpy(&ver, s->base + 4, 4);
    uint32_t kind; memcpy(&kind, s->base + 8, 4);
    if(ver != 1 || kind != 1) FAIL("unsupported .thim version or kind");
    /* u32 fields start at byte 16 */
    int i = 0;
    s->d = (int)h32(s, i++); s->nh = (int)h32(s, i++); s->ne = (int)h32(s, i++); s->nd = (int)h32(s, i++);
    s->ffn = (int)h32(s, i++); s->nm = (int)h32(s, i++); s->vocab = (int)h32(s, i++);
    s->n_audio = (int)h32(s, i++); s->n_text = (int)h32(s, i++);
    s->n_fft = (int)h32(s, i++); s->hop = (int)h32(s, i++); s->rate = (int)h32(s, i++);
    s->sot = (int)h32(s, i++); s->eot = (int)h32(s, i++); s->notimestamps = (int)h32(s, i++);
    int ntab = (int)h32(s, i++); s->nsup = (int)h32(s, i++);
    uint64_t table_off = h64(s, 0), toff = h64(s, 1), tblob = h64(s, 2), supoff = h64(s, 3);
    if(s->ne > 8 || s->nd > 8 || s->d % s->nh || s->n_fft != 400 || s->nm != 80) FAIL("model size not supported");
    if(table_off + 96ull * ntab > len) FAIL("truncated file");
    s->toff = (const uint32_t *)(s->base + toff); s->tblob = s->base + tblob; s->sup = (const int32_t *)(s->base + supoff);

    loader_t L = { s, ntab, s->base + table_off, 0, NULL };
    s->mel = L_f32(&L, "mel"); s->window = L_f32(&L, "window");
    s->conv1 = L_q8(&L, "enc.conv1.w"); s->conv1b = L_f32(&L, "enc.conv1.b");
    s->conv2 = L_q8(&L, "enc.conv2.w"); s->conv2b = L_f32(&L, "enc.conv2.b");
    s->enc_pos = L_f32(&L, "enc.pos");
    char n[80];
    for(int l = 0; l < s->ne; l++){
        layer_t *y = &s->enc[l];
        mkname(n, sizeof n, "enc.", l, ".ln1"); y->ln1 = L_ln(&L, n);
        mkname(n, sizeof n, "enc.", l, ".sa");  y->sa = L_attn(&L, n);
        mkname(n, sizeof n, "enc.", l, ".ln2"); y->ln2 = L_ln(&L, n);
        mkname(n, sizeof n, "enc.", l, ".fc1.w"); y->fc1 = L_q8(&L, n);
        mkname(n, sizeof n, "enc.", l, ".fc1.b"); y->b1 = L_f32(&L, n);
        mkname(n, sizeof n, "enc.", l, ".fc2.w"); y->fc2 = L_q8(&L, n);
        mkname(n, sizeof n, "enc.", l, ".fc2.b"); y->b2 = L_f32(&L, n);
    }
    s->enc_ln = L_ln(&L, "enc.ln");
    s->emb = L_q8(&L, "dec.emb"); s->dec_pos = L_f32(&L, "dec.pos");
    for(int l = 0; l < s->nd; l++){
        layer_t *y = &s->dec[l];
        mkname(n, sizeof n, "dec.", l, ".ln1"); y->ln1 = L_ln(&L, n);
        mkname(n, sizeof n, "dec.", l, ".sa");  y->sa = L_attn(&L, n);
        mkname(n, sizeof n, "dec.", l, ".lnx"); y->lnx = L_ln(&L, n);
        mkname(n, sizeof n, "dec.", l, ".xa");  y->xa = L_attn(&L, n);
        mkname(n, sizeof n, "dec.", l, ".ln2"); y->ln2 = L_ln(&L, n);
        mkname(n, sizeof n, "dec.", l, ".fc1.w"); y->fc1 = L_q8(&L, n);
        mkname(n, sizeof n, "dec.", l, ".fc1.b"); y->b1 = L_f32(&L, n);
        mkname(n, sizeof n, "dec.", l, ".fc2.w"); y->fc2 = L_q8(&L, n);
        mkname(n, sizeof n, "dec.", l, ".fc2.b"); y->b2 = L_f32(&L, n);
    }
    s->dec_ln = L_ln(&L, "dec.ln");
    if(L.bad){ copy_err(err, err_len, "missing tensor"); thim_stt_close(s); return NULL; }

    /* the DFT as two dense tables: row k holds cos / sin of 2 pi k n / 400 */
    float *tc = xalloc(400 * sizeof(float)), *ts = xalloc(400 * sizeof(float));
    s->dft_c = xalloc(201 * 400 * sizeof(float)); s->dft_s = xalloc(201 * 400 * sizeof(float));
    s->suppress = xalloc((size_t)s->vocab);
    if(!tc || !ts || !s->dft_c || !s->dft_s || !s->suppress){ if(tc) g_free(tc); if(ts) g_free(ts); FAIL("out of memory"); }
    tiny_trig(tc, ts, 400);
    for(int k = 0; k < 201; k++)
        for(int j = 0; j < 400; j++){ int m = (k * j) % 400; s->dft_c[k * 400 + j] = tc[m]; s->dft_s[k * 400 + j] = ts[m]; }
    g_free(tc); g_free(ts);
    for(int j = 0; j < s->nsup; j++) if(s->sup[j] >= 0 && s->sup[j] < s->vocab) s->suppress[s->sup[j]] = 1;
    for(int t = s->notimestamps + 1; t < s->vocab; t++) s->suppress[t] = 1;      /* no timestamp tokens */
    for(int t = s->eot + 1; t <= s->notimestamps && t < s->vocab; t++) s->suppress[t] = 1;   /* nor other specials */
    return s;
    #undef FAIL
}

static void free_scratch(thim_stt *s){
    float **ptrs[] = { &s->mel_buf, &s->x, &s->h, &s->q, &s->k, &s->v, &s->ctx, &s->f, &s->scores, &s->qs };
    for(unsigned i = 0; i < sizeof ptrs / sizeof *ptrs; i++){ if(*ptrs[i]) g_free(*ptrs[i]); *ptrs[i] = NULL; }
    if(s->qa){ g_free(s->qa); s->qa = NULL; }
    for(int l = 0; l < 8; l++){
        if(s->xk[l]) g_free(s->xk[l]); if(s->xv[l]) g_free(s->xv[l]);
        if(s->kc[l]) g_free(s->kc[l]); if(s->vc[l]) g_free(s->vc[l]);
        s->xk[l] = s->xv[l] = s->kc[l] = s->vc[l] = NULL;
    }
    s->cap_T = 0; s->qa_cap = 0;
}

void thim_stt_close(thim_stt *s){
    if(!s) return;
    free_scratch(s);
    if(s->dft_c) g_free(s->dft_c);
    if(s->dft_s) g_free(s->dft_s);
    if(s->suppress) g_free(s->suppress);
    g_free(s);
}

int thim_stt_sample_rate(const thim_stt *s){ return s->rate; }
void thim_stt_stats_get(const thim_stt *s, thim_stt_stats *st){ *st = s->st; }

static double now(void){ return g_clock ? g_clock() : 0.0; }
double thim_stt_prof[8];                                /* mm, attention, gelu, layernorm, conv glue (microseconds) */
#define PROF_BEGIN double _p0 = now()
#define PROF_END(i) thim_stt_prof[i] += now() - _p0

/* ---------------- kernels ---------------- */

/* activations -> int8 in blocks of 32 (zero padded to cp); one float scale per block */
static void quant_rows(const float *A, int M, int K, int cp, int8_t *q, float *sc){
    int nb = cp / BLOCK;
    for(int m = 0; m < M; m++){
        const float *a = A + (size_t)m * K;
        int8_t *qr = q + (size_t)m * cp;
        float *sr = sc + (size_t)m * nb;
        for(int b = 0; b < nb; b++){
            float t[BLOCK]; float amax = 0.f;
            for(int i = 0; i < BLOCK; i++){ int idx = b * BLOCK + i; float v = idx < K ? a[idx] : 0.f; t[i] = v; float av = v < 0 ? -v : v; if(av > amax) amax = av; }
            float scale = amax > 0.f ? amax / 127.f : 1.f, inv = amax > 0.f ? 127.f / amax : 0.f;
            sr[b] = scale;
            for(int i = 0; i < BLOCK; i++){ float r = t[i] * inv; qr[b * BLOCK + i] = (int8_t)(int)(r < 0 ? r - 0.5f : r + 0.5f); }
        }
    }
}

/* sum of 32 int8 products */
static inline int dot32(const int8_t *a, const int8_t *b){
#if defined(__AVX2__)
    __m256i x = _mm256_loadu_si256((const __m256i *)a), y = _mm256_loadu_si256((const __m256i *)b);
    __m256i ax = _mm256_sign_epi8(x, x);                   /* |x| */
    __m256i sy = _mm256_sign_epi8(y, x);                   /* y with x's sign */
    __m256i p = _mm256_maddubs_epi16(ax, sy);              /* pairs of u8*s8 -> s16 */
    __m256i s = _mm256_madd_epi16(p, _mm256_set1_epi16(1));
    __m128i lo = _mm256_castsi256_si128(s), hi = _mm256_extracti128_si256(s, 1);
    __m128i t = _mm_add_epi32(lo, hi);
    t = _mm_add_epi32(t, _mm_shuffle_epi32(t, 0x4E)); t = _mm_add_epi32(t, _mm_shuffle_epi32(t, 0xB1));
    return _mm_cvtsi128_si32(t);
#elif defined(__SSE4_1__)
    __m128i acc = _mm_setzero_si128();
    for(int i = 0; i < 32; i += 16){
        __m128i x = _mm_loadu_si128((const __m128i *)(a + i)), y = _mm_loadu_si128((const __m128i *)(b + i));
        __m128i xl = _mm_cvtepi8_epi16(x), yl = _mm_cvtepi8_epi16(y);
        __m128i xh = _mm_cvtepi8_epi16(_mm_srli_si128(x, 8)), yh = _mm_cvtepi8_epi16(_mm_srli_si128(y, 8));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(xl, yl));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(xh, yh));
    }
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0x4E)); acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0xB1));
    return _mm_cvtsi128_si32(acc);
#else
    int s = 0;
    for(int i = 0; i < BLOCK; i++) s += (int)a[i] * (int)b[i];
    return s;
#endif
}

static int ensure_q(thim_stt *s, size_t M, size_t cp){
    size_t need = M * cp;
    if(need <= s->qa_cap) return 1;
    if(s->qa) g_free(s->qa);
    if(s->qs) g_free(s->qs);
    s->qa = xalloc(need); s->qs = xalloc(need / BLOCK * sizeof(float) + 64);
    s->qa_cap = s->qa && s->qs ? need : 0;
    return s->qa_cap != 0;
}

/* Four rows of activations against one row of weights: the weight block is loaded and its
 * absolute value taken once, then each activation block costs one sign, one multiply-add of
 * bytes, one of words and one float multiply-add. The float sums stay in vector registers
 * across all blocks; there is one horizontal add per output, not per block. */
#if defined(__AVX2__) || defined(__SSSE3__)
static inline float hsum_ps128(__m128 v){ v = _mm_add_ps(v, _mm_movehl_ps(v, v)); v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1)); return _mm_cvtss_f32(v); }
#endif

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

/* One row of activations against four rows of weights (the decoder: one token at a time). */
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

/* out[M][N] = A[M][K] W^T + bias.  The activations are quantized once; rows of W stream once per tile of rows of A. */
static void mm(thim_stt *s, float *out, const float *A, int M, const mat_t *W, const float *bias){
    PROF_BEGIN;
    int K = (int)W->cols, cp = (int)W->cp, N = (int)W->rows, nb = cp / BLOCK;
    if(!ensure_q(s, (size_t)M + 4, (size_t)cp)) return;
    quant_rows(A, M, K, cp, s->qa, s->qs);
    if(M < 4){                                              /* a token at a time: four weight rows per pass */
        for(int m = 0; m < M; m++){
            const int8_t *ar = s->qa + (size_t)m * cp; const float *as = s->qs + (size_t)m * nb;
            for(int n = 0; n < N; n += 4){
                const int8_t *wp[4]; const float *sp[4]; float r[4];
                for(int i = 0; i < 4; i++){ int ni = n + i < N ? n + i : N - 1; wp[i] = W->q + (size_t)ni * cp; sp[i] = W->sc + (size_t)ni * nb; }
                rows4(wp, sp, ar, as, nb, r);
                for(int i = 0; i < 4 && n + i < N; i++) out[(size_t)m * N + n + i] = r[i] + (bias ? bias[n + i] : 0.f);
            }
        }
        PROF_END(0);
        return;
    }
    /* the last tile may be short: point its spare rows at the last real one (the results are ignored) */
    const int TILE = 32;
    for(int m0 = 0; m0 < M; m0 += TILE){
        int m1 = m0 + TILE < M ? m0 + TILE : M;
        for(int n = 0; n < N; n++){
            const int8_t *wr = W->q + (size_t)n * cp;
            const float *ws = W->sc + (size_t)n * nb;
            float bv = bias ? bias[n] : 0.f;
            for(int m = m0; m < m1; m += 4){
                const int8_t *ap[4]; const float *sp[4]; float r[4];
                for(int i = 0; i < 4; i++){ int mi = m + i < m1 ? m + i : m1 - 1; ap[i] = s->qa + (size_t)mi * cp; sp[i] = s->qs + (size_t)mi * nb; }
                tile4(wr, ws, ap, sp, nb, r);
                for(int i = 0; i < 4 && m + i < m1; i++) out[(size_t)(m + i) * N + n] = r[i] + bv;
            }
        }
    }
    PROF_END(0);
}

static void layernorm(float *dst, const float *src, int M, int D, const ln_t *l){
    PROF_BEGIN;
    for(int m = 0; m < M; m++){
        const float *x = src + (size_t)m * D; float *y = dst + (size_t)m * D;
        float mean = 0.f;
        for(int i = 0; i < D; i++) mean += x[i];
        mean /= (float)D;
        float var = 0.f;
        for(int i = 0; i < D; i++){ float t = x[i] - mean; var += t * t; }
        var /= (float)D;
        float inv = 1.f / fsqrt(var + 1e-5f);
        for(int i = 0; i < D; i++) y[i] = (x[i] - mean) * inv * l->w[i] + l->b[i];
    }
    PROF_END(3);
}

static inline float fdot(const float *a, const float *b, int n){
#if defined(__AVX2__)
    __m256 acc0 = _mm256_setzero_ps(), acc1 = acc0;
    int i = 0;
    for(; i + 16 <= n; i += 16){
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(acc0), _mm256_extractf128_ps(acc0, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h)); h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    float r = _mm_cvtss_f32(h);
    for(; i < n; i++) r += a[i] * b[i];
    return r;
#elif defined(__SSE2__)
    __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps();
    int i = 0;
    for(; i + 8 <= n; i += 8){
        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    acc0 = _mm_add_ps(acc0, acc1);
    float t[4]; _mm_storeu_ps(t, acc0);
    float r = t[0] + t[1] + t[2] + t[3];
    for(; i < n; i++) r += a[i] * b[i];
    return r;
#else
    float r = 0.f; for(int i = 0; i < n; i++) r += a[i] * b[i]; return r;
#endif
}

/* multi-head attention for rows [0, Tq) of q against Tk keys; causal: query i sees keys 0..i0+i */
static void attention(thim_stt *s, float *ctx, const float *q, const float *k, const float *v, int Tq, int Tk, int causal, int i0){
    PROF_BEGIN;
    int D = s->d, H = s->nh, hd = D / H;
    float *sc = s->scores;
    for(int i = 0; i < Tq; i++){
        int lim = causal ? i0 + i + 1 : Tk;
        if(lim > Tk) lim = Tk;
        for(int h = 0; h < H; h++){
            const float *qi = q + (size_t)i * D + h * hd;
            float mx = -1e30f;
            for(int j = 0; j < lim; j++){ float d = fdot(qi, k + (size_t)j * D + h * hd, hd); sc[j] = d; if(d > mx) mx = d; }
            float sum = softmax_exp(sc, lim, mx);
            float inv = 1.f / sum;
            float *o = ctx + (size_t)i * D + h * hd;
            for(int c = 0; c < hd; c++) o[c] = 0.f;
            for(int j = 0; j < lim; j++){
                float p = sc[j] * inv;
                const float *vj = v + (size_t)j * D + h * hd;
                for(int c = 0; c < hd; c++) o[c] += p * vj[c];
            }
        }
    }
    PROF_END(1);
}

/* ---------------- front end: samples -> log-mel frames ---------------- */
static void log_mel(thim_stt *s, const float *pcm, int n, int T_mel, float *out /* [T_mel][80] */){
    const int NF = 400, HOP = 160, NK = 201;
    int L = T_mel * HOP;                                   /* the audio, zero padded to what the encoder will see */
    float *sig = xalloc(((size_t)L + NF) * sizeof(float));
    float *frame = xalloc(NF * sizeof(float)), *pw = xalloc(NK * sizeof(float));
    if(!sig || !frame || !pw){ if(sig) g_free(sig); if(frame) g_free(frame); if(pw) g_free(pw); return; }
    for(int i = 0; i < L; i++) sig[i] = i < n ? pcm[i] : 0.f;
    float gmax = -1e30f;
    for(int t = 0; t < T_mel; t++){
        for(int j = 0; j < NF; j++){                       /* centred frame, reflected at the ends */
            int idx = t * HOP + j - NF / 2;
            if(idx < 0) idx = -idx;
            if(idx >= L) idx = 2 * (L - 1) - idx;
            if(idx < 0) idx = 0;
            frame[j] = sig[idx] * s->window[j];
        }
        for(int k = 0; k < NK; k++){
            const float *c = s->dft_c + (size_t)k * NF, *sn = s->dft_s + (size_t)k * NF;
            float re = fdot(frame, c, NF), im = fdot(frame, sn, NF);
            pw[k] = re * re + im * im;
        }
        for(int m = 0; m < 80; m++){
            float acc = fdot(pw, s->mel + (size_t)m * NK, NK);
            if(acc < 1e-10f) acc = 1e-10f;
            float lg = flog2(acc) * 0.30102999566f;       /* log10 */
            out[(size_t)t * 80 + m] = lg;
            if(lg > gmax) gmax = lg;
        }
    }
    float floor_ = gmax - 8.f;
    for(size_t i = 0; i < (size_t)T_mel * 80; i++){ float v = out[i] < floor_ ? floor_ : out[i]; out[i] = (v + 4.f) * 0.25f; }
    g_free(sig); g_free(frame); g_free(pw);
}

/* ---------------- scratch ---------------- */
static int ensure_T(thim_stt *s, size_t T){
    if(T <= s->cap_T) return 1;
    free_scratch(s);
    int D = s->d;
    size_t Td = (size_t)s->n_text;
    s->mel_buf = xalloc(T * 2 * 80 * sizeof(float));
    s->x = xalloc(T * D * sizeof(float)); s->h = xalloc(T * D * sizeof(float));
    s->q = xalloc(T * D * sizeof(float)); s->k = xalloc(T * D * sizeof(float)); s->v = xalloc(T * D * sizeof(float));
    s->ctx = xalloc(T * D * sizeof(float));
    s->f = xalloc(T * (size_t)s->ffn * sizeof(float));
    s->scores = xalloc((T > Td ? T : Td) * sizeof(float));
    if(!s->mel_buf || !s->x || !s->h || !s->q || !s->k || !s->v || !s->ctx || !s->f || !s->scores) return 0;
    for(int l = 0; l < s->nd; l++){
        s->xk[l] = xalloc(T * D * sizeof(float)); s->xv[l] = xalloc(T * D * sizeof(float));
        s->kc[l] = xalloc(Td * D * sizeof(float)); s->vc[l] = xalloc(Td * D * sizeof(float));
        if(!s->xk[l] || !s->xv[l] || !s->kc[l] || !s->vc[l]) return 0;
    }
    s->cap_T = T;
    return 1;
}


/* ---------------- encoder ---------------- */
static void encode(thim_stt *s, int Te){
    int D = s->d, T_mel = Te * 2;
    float *x = s->x, *h = s->h;
    /* conv1: kernel 3, padding 1: each output frame reads three mel frames (zero outside), unrolled into a row of 240 */
    float *c1buf = xalloc((size_t)T_mel * D * sizeof(float));
    float *pbuf = xalloc((size_t)T_mel * 240 * sizeof(float));
    if(!c1buf || !pbuf){ if(c1buf) g_free(c1buf); if(pbuf) g_free(pbuf); return; }
    for(int t = 0; t < T_mel; t++){
        float *p = pbuf + (size_t)t * 240;
        for(int k = 0; k < 3; k++){
            int fr = t + k - 1;
            if(fr < 0 || fr >= T_mel) memset(p + k * 80, 0, 80 * sizeof(float));
            else memcpy(p + k * 80, s->mel_buf + (size_t)fr * 80, 80 * sizeof(float));
        }
    }
    mm(s, c1buf, pbuf, T_mel, &s->conv1, s->conv1b);
    gelu_inplace(c1buf, (size_t)T_mel * D);
    g_free(pbuf);
    float *p2 = xalloc((size_t)Te * 3 * D * sizeof(float));
    if(!p2){ g_free(c1buf); return; }
    for(int u = 0; u < Te; u++){
        for(int k = 0; k < 3; k++){
            int fr = 2 * u + k - 1;
            float *dst = p2 + (size_t)u * 3 * D + (size_t)k * D;
            if(fr < 0 || fr >= T_mel) memset(dst, 0, (size_t)D * sizeof(float));
            else memcpy(dst, c1buf + (size_t)fr * D, (size_t)D * sizeof(float));
        }
    }
    g_free(c1buf);
    mm(s, x, p2, Te, &s->conv2, s->conv2b);
    g_free(p2);
    for(int u = 0; u < Te; u++)
        for(int i = 0; i < D; i++){ size_t idx = (size_t)u * D + i; x[idx] = gelu(x[idx]) + s->enc_pos[idx]; }

    for(int l = 0; l < s->ne; l++){
        const layer_t *y = &s->enc[l];
        layernorm(h, x, Te, D, &y->ln1);
        mm(s, s->q, h, Te, &y->sa.q, y->sa.qb);
        mm(s, s->k, h, Te, &y->sa.k, NULL);
        mm(s, s->v, h, Te, &y->sa.v, y->sa.vb);
        for(size_t i = 0; i < (size_t)Te * D; i++) s->q[i] *= 0.125f;      /* head_dim ** -0.5 */
        attention(s, s->ctx, s->q, s->k, s->v, Te, Te, 0, 0);
        mm(s, h, s->ctx, Te, &y->sa.o, y->sa.ob);
        for(size_t i = 0; i < (size_t)Te * D; i++) x[i] += h[i];
        layernorm(h, x, Te, D, &y->ln2);
        mm(s, s->f, h, Te, &y->fc1, y->b1);
        { PROF_BEGIN; gelu_inplace(s->f, (size_t)Te * s->ffn); PROF_END(2); }
        mm(s, h, s->f, Te, &y->fc2, y->b2);
        for(size_t i = 0; i < (size_t)Te * D; i++) x[i] += h[i];
    }
    layernorm(x, x, Te, D, &s->enc_ln);
    /* the cross-attention keys and values, once for the whole utterance */
    for(int l = 0; l < s->nd; l++){
        const attn_t *a = &s->dec[l].xa;
        mm(s, s->xk[l], x, Te, &a->k, NULL);
        mm(s, s->xv[l], x, Te, &a->v, a->vb);
    }
}

/* ---------------- decoder ---------------- */
/* one token at position pos; returns the logits (valid until the next call) in out[vocab] */
static void dec_step(thim_stt *s, int token, int pos, int Te, float *logits){
    int D = s->d;
    float x[512], h[512], q[512], ctx[512], t1[512], f[2048];
    /* the embedding: dequantize one row */
    {
        const int8_t *r = s->emb.q + (size_t)token * s->emb.cp; const float *sc = s->emb.sc + (size_t)token * (s->emb.cp / BLOCK);
        for(int i = 0; i < D; i++) x[i] = (float)r[i] * sc[i / BLOCK] + s->dec_pos[(size_t)pos * D + i];
    }
    for(int l = 0; l < s->nd; l++){
        const layer_t *y = &s->dec[l];
        layernorm(h, x, 1, D, &y->ln1);
        mm(s, q, h, 1, &y->sa.q, y->sa.qb);
        mm(s, s->kc[l] + (size_t)pos * D, h, 1, &y->sa.k, NULL);
        mm(s, s->vc[l] + (size_t)pos * D, h, 1, &y->sa.v, y->sa.vb);
        for(int i = 0; i < D; i++) q[i] *= 0.125f;
        attention(s, ctx, q, s->kc[l], s->vc[l], 1, pos + 1, 1, pos);
        mm(s, t1, ctx, 1, &y->sa.o, y->sa.ob);
        for(int i = 0; i < D; i++) x[i] += t1[i];
        layernorm(h, x, 1, D, &y->lnx);
        mm(s, q, h, 1, &y->xa.q, y->xa.qb);
        for(int i = 0; i < D; i++) q[i] *= 0.125f;
        attention(s, ctx, q, s->xk[l], s->xv[l], 1, Te, 0, 0);
        mm(s, t1, ctx, 1, &y->xa.o, y->xa.ob);
        for(int i = 0; i < D; i++) x[i] += t1[i];
        layernorm(h, x, 1, D, &y->ln2);
        mm(s, f, h, 1, &y->fc1, y->b1);
        for(int i = 0; i < s->ffn; i++) f[i] = gelu(f[i]);
        mm(s, t1, f, 1, &y->fc2, y->b2);
        for(int i = 0; i < D; i++) x[i] += t1[i];
    }
    layernorm(h, x, 1, D, &s->dec_ln);
    mm(s, logits, h, 1, &s->emb, NULL);                    /* the embedding matrix is also the output matrix */
}

int thim_stt_transcribe(thim_stt *s, const float *pcm, int n_samples, const thim_stt_opts *opts, char *out, size_t out_cap){
    if(!s || !out || !out_cap) return -1;
    out[0] = 0;
    thim_stt_opts o = { 224, 100, 25, 0 };
    if(opts){ if(opts->max_tokens > 0) o.max_tokens = opts->max_tokens; if(opts->min_ctx > 0) o.min_ctx = opts->min_ctx; if(opts->pad_ctx > 0) o.pad_ctx = opts->pad_ctx; o.full_ctx = opts->full_ctx; }
    memset(&s->st, 0, sizeof s->st);
    if(n_samples < 1600) return 0;                         /* under 0.1 s: nothing to hear */
    int Te = (int)((long long)n_samples * 50 / s->rate) + o.pad_ctx;
    if(Te < o.min_ctx) Te = o.min_ctx;
    if(o.full_ctx || Te > s->n_audio) Te = s->n_audio;
    if(n_samples > Te * 320) n_samples = Te * 320;
    if(!ensure_T(s, (size_t)Te)) return -1;

    double t0 = now();
    log_mel(s, pcm, n_samples, Te * 2, s->mel_buf);
    double t1 = now();
    encode(s, Te);
    double t2 = now();

    float *logits = xalloc((size_t)s->vocab * sizeof(float));
    if(!logits) return -1;
    int toks[512], nt = 0;
    int cur = s->sot, pos = 0, ntok = 0, rep = 0;
    int prompt[2] = { s->sot, s->notimestamps };
    int maxn = o.max_tokens < s->n_text - 4 ? o.max_tokens : s->n_text - 4;
    /* speech runs about 3 words (4-5 tokens) a second: far more than that is the decoder looping */
    int budget = 10 + (int)((long long)n_samples * 7 / s->rate);
    if(maxn > budget) maxn = budget;
    for(int step = 0; step < maxn + 2; step++){
        dec_step(s, cur, pos, Te, logits);
        pos++;
        if(step < 1){ cur = prompt[step + 1]; continue; }   /* feed <|notimestamps|> after <|startoftranscript|> */
        /* greedy over the allowed tokens */
        if(step == 1){ logits[220] = -1e30f; logits[s->eot] = -1e30f; }
        int best = -1; float bv = -1e30f;
        for(int t = 0; t < s->vocab; t++){
            if(s->suppress[t] && t != s->eot) continue;
            if(logits[t] > bv){ bv = logits[t]; best = t; }
        }
        if(best < 0 || best == s->eot) break;
        if(ntok && toks[ntok - 1] == best){ if(++rep >= 6) break; } else rep = 0;   /* a stuck loop */
        toks[ntok++] = best;
        cur = best;
        /* a phrase said again straight after itself (2..40 tokens): keep it once and stop */
        { int loop = 0;
          for(int k = 2; k <= 40 && 2 * k <= ntok && !loop; k++)
              if(!memcmp(toks + ntok - k, toks + ntok - 2 * k, (size_t)k * sizeof(int))){ ntok -= k; loop = 1; }
          if(loop) break; }
        if(ntok >= 500) break;
    }
    double t3 = now();
    g_free(logits);

    size_t w = 0; int first = 1;
    for(int i = 0; i < ntok; i++){
        int t = toks[i];
        uint32_t a = s->toff[t], b = s->toff[t + 1];
        for(uint32_t k = a; k < b; k++){
            unsigned char c = s->tblob[k];
            if(first && c == ' ') continue;
            first = 0;
            if(w + 1 < out_cap) out[w++] = (char)c;
        }
    }
    out[w] = 0;
    s->st.mel_us = t1 - t0; s->st.encoder_us = t2 - t1; s->st.decoder_us = t3 - t2; s->st.enc_frames = Te; s->st.tokens = ntok;
    return (int)w;
}
