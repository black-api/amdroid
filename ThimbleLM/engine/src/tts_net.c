/* ThimbleTTS networks in C: duration -> acoustic -> ConvNeXt decoder -> iSTFT.
 * Mirrors thimble/tts/models.py layer for layer. All tensors are channel-first [C][T]. */
#include "tts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ safetensors */
static void set_err(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}

typedef struct { const char *p, *e; } jr;

static void jws(jr *r) { while (r->p < r->e && (*r->p == ' ' || *r->p == '\n' || *r->p == '\t' || *r->p == '\r')) r->p++; }

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static int put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

/* parse a JSON string at r->p (which points at the opening quote); returns malloc'd UTF-8 */
static char *jstr(jr *r) {
    if (r->p >= r->e || *r->p != '"') return NULL;
    r->p++;
    char *out = malloc((size_t)(r->e - r->p) + 1), *o = out;
    if (!out) return NULL;
    while (r->p < r->e && *r->p != '"') {
        if (*r->p == '\\' && r->p + 1 < r->e) {
            char c = *++r->p; r->p++;
            switch (c) {
            case 'n': *o++ = '\n'; break;  case 't': *o++ = '\t'; break;  case 'r': *o++ = '\r'; break;
            case 'b': *o++ = '\b'; break;  case 'f': *o++ = '\f'; break;
            case 'u': {
                if (r->p + 4 > r->e) { free(out); return NULL; }
                int cp = hex4(r->p); r->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && r->p + 6 <= r->e && r->p[0] == '\\' && r->p[1] == 'u') {
                    int lo = hex4(r->p + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); r->p += 6; }
                }
                if (cp < 0) { free(out); return NULL; }
                o += put_utf8(o, (unsigned)cp);
                break;
            }
            default: *o++ = c;
            }
        } else *o++ = *r->p++;
    }
    if (r->p >= r->e) { free(out); return NULL; }
    r->p++;
    *o = 0;
    return out;
}

static int jskip(jr *r);
static int jskip_container(jr *r, char close) {
    r->p++;
    jws(r);
    if (r->p < r->e && *r->p == close) { r->p++; return 0; }
    for (;;) {
        jws(r);
        if (close == '}') {
            char *k = jstr(r); if (!k) return -1; free(k);
            jws(r); if (r->p >= r->e || *r->p != ':') return -1; r->p++;
        }
        if (jskip(r)) return -1;
        jws(r);
        if (r->p < r->e && *r->p == ',') { r->p++; continue; }
        if (r->p < r->e && *r->p == close) { r->p++; return 0; }
        return -1;
    }
}
static int jskip(jr *r) {
    jws(r);
    if (r->p >= r->e) return -1;
    if (*r->p == '"') { char *s = jstr(r); if (!s) return -1; free(s); return 0; }
    if (*r->p == '{') return jskip_container(r, '}');
    if (*r->p == '[') return jskip_container(r, ']');
    while (r->p < r->e && *r->p != ',' && *r->p != '}' && *r->p != ']') r->p++;
    return 0;
}

static int parse_tensor(jr *r, tts_tensor *t) {
    jws(r); if (*r->p != '{') return -1; r->p++;
    t->ndim = 0; t->dtype = -1; t->off = t->end = 0;
    for (;;) {
        jws(r);
        if (*r->p == '}') { r->p++; return t->dtype < 0 ? -1 : 0; }
        char *k = jstr(r); if (!k) return -1;
        jws(r); if (*r->p != ':') { free(k); return -1; } r->p++; jws(r);
        if (!strcmp(k, "dtype")) {
            char *d = jstr(r);
            if (!d) { free(k); return -1; }
            t->dtype = !strcmp(d, "F32") ? TT_F32 : !strcmp(d, "I32") ? TT_I32 : !strcmp(d, "U8") ? TT_U8 : -1;
            free(d);
        } else if (!strcmp(k, "shape") || !strcmp(k, "data_offsets")) {
            int is_shape = k[0] == 's';
            if (*r->p != '[') { free(k); return -1; }
            r->p++;
            long long vals[8]; int nv = 0;
            for (;;) {
                jws(r);
                if (*r->p == ']') { r->p++; break; }
                char *end; long long x = strtoll(r->p, &end, 10);
                if (end == r->p || nv >= 8) { free(k); return -1; }
                vals[nv++] = x; r->p = end; jws(r);
                if (*r->p == ',') r->p++;
            }
            if (is_shape) { t->ndim = nv > 4 ? 4 : nv; for (int i = 0; i < t->ndim; i++) t->shape[i] = (int)vals[i]; }
            else if (nv == 2) { t->off = (size_t)vals[0]; t->end = (size_t)vals[1]; }
        } else if (jskip(r)) { free(k); return -1; }
        free(k);
        jws(r); if (*r->p == ',') r->p++;
    }
}

int tts_file_open(tts_file *f, const char *path, char *err, size_t err_len) {
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(path, "rb");
    if (!fp) { set_err(err, err_len, "cannot open voice file %s", path); return -1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 16) { fclose(fp); set_err(err, err_len, "%s is not a voice file", path); return -1; }
    f->buf = malloc((size_t)sz);
    if (!f->buf || fread(f->buf, 1, (size_t)sz, fp) != (size_t)sz) { fclose(fp); tts_file_close(f); set_err(err, err_len, "cannot read %s", path); return -1; }
    fclose(fp);
    f->size = (size_t)sz;
    uint64_t hl = 0;
    for (int i = 7; i >= 0; i--) hl = hl << 8 | f->buf[i];
    if (hl + 8 > f->size) { tts_file_close(f); set_err(err, err_len, "%s: corrupt header", path); return -1; }
    f->data0 = 8 + hl;
    jr r = { (const char *)f->buf + 8, (const char *)f->buf + 8 + hl };
    jws(&r);
    if (*r.p != '{') { tts_file_close(f); set_err(err, err_len, "%s: bad header", path); return -1; }
    r.p++;
    int cap_t = 0, cap_m = 0;
    for (;;) {
        jws(&r);
        if (r.p >= r.e || *r.p == '}') break;
        char *k = jstr(&r);
        jws(&r);
        if (!k || *r.p != ':') { free(k); tts_file_close(f); set_err(err, err_len, "%s: bad header", path); return -1; }
        r.p++; jws(&r);
        if (!strcmp(k, "__metadata__")) {
            free(k);
            if (*r.p != '{') { tts_file_close(f); return -1; }
            r.p++;
            for (;;) {
                jws(&r);
                if (*r.p == '}') { r.p++; break; }
                char *mk = jstr(&r); jws(&r);
                if (!mk || *r.p != ':') { free(mk); tts_file_close(f); return -1; }
                r.p++; jws(&r);
                char *mv = jstr(&r);
                if (!mv) { free(mk); tts_file_close(f); return -1; }
                if (f->nm == cap_m) { cap_m = cap_m ? cap_m * 2 : 32; f->m = realloc(f->m, (size_t)cap_m * sizeof *f->m); }
                f->m[f->nm].key = mk; f->m[f->nm].val = mv; f->nm++;
                jws(&r); if (*r.p == ',') r.p++;
            }
        } else {
            tts_tensor t; memset(&t, 0, sizeof t); t.name = k;
            if (parse_tensor(&r, &t) || f->data0 + t.end > f->size) { free(k); tts_file_close(f); set_err(err, err_len, "%s: bad tensor entry", path); return -1; }
            if (f->nt == cap_t) { cap_t = cap_t ? cap_t * 2 : 128; f->t = realloc(f->t, (size_t)cap_t * sizeof *f->t); }
            f->t[f->nt++] = t;
        }
        jws(&r); if (*r.p == ',') r.p++;
    }
    return 0;
}

void tts_file_close(tts_file *f) {
    for (int i = 0; i < f->nt; i++) free(f->t[i].name);
    for (int i = 0; i < f->nm; i++) { free(f->m[i].key); free(f->m[i].val); }
    free(f->t); free(f->m); free(f->buf);
    memset(f, 0, sizeof *f);
}

const tts_tensor *tts_file_find(const tts_file *f, const char *name) {
    for (int i = 0; i < f->nt; i++) if (!strcmp(f->t[i].name, name)) return &f->t[i];
    return NULL;
}
const void *tts_file_data(const tts_file *f, const tts_tensor *t) { return f->buf + f->data0 + t->off; }
const char *tts_file_meta(const tts_file *f, const char *key) {
    for (int i = 0; i < f->nm; i++) if (!strcmp(f->m[i].key, key)) return f->m[i].val;
    return NULL;
}

/* ------------------------------------------------------------------ tensor lookup */
static const float *F(const thimble_voice *v, char *err, size_t n, const char *fmt, ...);
#include <stdarg.h>
static const float *F(const thimble_voice *v, char *err, size_t n, const char *fmt, ...) {
    char name[160];
    va_list ap; va_start(ap, fmt); vsnprintf(name, sizeof name, fmt, ap); va_end(ap);
    const tts_tensor *t = tts_file_find(&v->file, name);
    if (!t || t->dtype != TT_F32) { if (err && n && !err[0]) snprintf(err, n, "voice file is missing tensor %s", name); return NULL; }
    return (const float *)tts_file_data(&v->file, t);
}
static int shape_of(const thimble_voice *v, const char *name, int dim) {
    const tts_tensor *t = tts_file_find(&v->file, name);
    return (t && dim < t->ndim) ? t->shape[dim] : -1;
}
static int count_keys(const thimble_voice *v, const char *fmt) {
    char name[160]; int n = 0;
    for (;; n++) { snprintf(name, sizeof name, fmt, n); if (!tts_file_find(&v->file, name)) return n; }
}

int tts_net_init(thimble_voice *v, char *err, size_t en) {
    if (err) err[0] = 0;
    /* ---- duration ---- */
    v->dur.vocab = shape_of(v, "dur.embed.weight", 0);
    v->dur.H = shape_of(v, "dur.embed.weight", 1);
    v->dur.depth = count_keys(v, "dur.scales.%d");
    if (v->dur.H < 1 || v->dur.depth < 1) { snprintf(err, en, "voice file has no duration network"); return -1; }
    v->dur.embed = F(v, err, en, "dur.embed.weight");
    v->dur.proj_w = F(v, err, en, "dur.proj.weight"); v->dur.proj_b = F(v, err, en, "dur.proj.bias");
    v->dur.out_w = F(v, err, en, "dur.out.weight"); v->dur.out_b = F(v, err, en, "dur.out.bias");
    v->dur.blk = calloc((size_t)v->dur.depth, sizeof(tts_res));
    for (int i = 0; i < v->dur.depth && v->dur.blk; i++) {
        tts_res *b = &v->dur.blk[i];
        b->w0 = F(v, err, en, "dur.blocks.%d.0.weight", i); b->b0 = F(v, err, en, "dur.blocks.%d.0.bias", i);
        b->w1 = F(v, err, en, "dur.blocks.%d.1.weight", i); b->b1 = F(v, err, en, "dur.blocks.%d.1.bias", i);
        const float *s = F(v, err, en, "dur.scales.%d", i); b->scale = s ? s[0] : 0.5f;
    }
    /* ---- acoustic ---- */
    v->ac.vocab = shape_of(v, "ac.embed.weight", 0);
    v->ac.H = shape_of(v, "ac.embed.weight", 1);
    v->ac.n_mels = shape_of(v, "ac.out.weight", 0);
    v->ac.token_depth = count_keys(v, "ac.token_scales.%d");
    v->ac.depth = count_keys(v, "ac.frame_scales.%d");
    if (v->ac.H < 1 || v->ac.n_mels < 1) { snprintf(err, en, "voice file has no acoustic network"); return -1; }
    v->ac.embed = F(v, err, en, "ac.embed.weight");
    v->ac.tproj_w = F(v, err, en, "ac.tproj.weight"); v->ac.tproj_b = F(v, err, en, "ac.tproj.bias");
    v->ac.fproj_w = F(v, err, en, "ac.fproj.weight"); v->ac.fproj_b = F(v, err, en, "ac.fproj.bias");
    v->ac.out_w = F(v, err, en, "ac.out.weight"); v->ac.out_b = F(v, err, en, "ac.out.bias");
    v->ac.tblk = calloc((size_t)v->ac.token_depth + 1, sizeof(tts_res));
    v->ac.fblk = calloc((size_t)v->ac.depth + 1, sizeof(tts_res));
    for (int i = 0; i < v->ac.token_depth && v->ac.tblk; i++) {
        tts_res *b = &v->ac.tblk[i];
        b->w0 = F(v, err, en, "ac.token_blocks.%d.0.weight", i); b->b0 = F(v, err, en, "ac.token_blocks.%d.0.bias", i);
        b->w1 = F(v, err, en, "ac.token_blocks.%d.1.weight", i); b->b1 = F(v, err, en, "ac.token_blocks.%d.1.bias", i);
        const float *s = F(v, err, en, "ac.token_scales.%d", i); b->scale = s ? s[0] : 0.5f;
    }
    for (int i = 0; i < v->ac.depth && v->ac.fblk; i++) {
        tts_res *b = &v->ac.fblk[i];
        b->w0 = F(v, err, en, "ac.frame_blocks.%d.0.weight", i); b->b0 = F(v, err, en, "ac.frame_blocks.%d.0.bias", i);
        b->w1 = F(v, err, en, "ac.frame_blocks.%d.1.weight", i); b->b1 = F(v, err, en, "ac.frame_blocks.%d.1.bias", i);
        const float *s = F(v, err, en, "ac.frame_scales.%d", i); b->scale = s ? s[0] : 0.5f;
    }
    /* ---- decoder ---- */
    v->dec.dim = shape_of(v, "dec.embed.weight", 0);
    v->dec.n_mels = shape_of(v, "dec.embed.weight", 1);
    v->dec.pw = shape_of(v, "dec.blocks.0.pw0.weight", 0);
    v->dec.bins = shape_of(v, "dec.head.weight", 0) / 2;
    v->dec.nblk = count_keys(v, "dec.blocks.%d.gamma");
    if (v->dec.dim < 1 || v->dec.nblk < 1 || v->dec.bins != 513) { snprintf(err, en, "voice file has no decoder network"); return -1; }
    v->dec.embed_w = F(v, err, en, "dec.embed.weight"); v->dec.embed_b = F(v, err, en, "dec.embed.bias");
    v->dec.noise_b = F(v, err, en, "dec.noise_adapter.bias");
    v->dec.norm_w = F(v, err, en, "dec.norm.weight"); v->dec.norm_b = F(v, err, en, "dec.norm.bias");
    v->dec.fnorm_w = F(v, err, en, "dec.fnorm.weight"); v->dec.fnorm_b = F(v, err, en, "dec.fnorm.bias");
    v->dec.head_w = F(v, err, en, "dec.head.weight"); v->dec.head_b = F(v, err, en, "dec.head.bias");
    v->dec.blk = calloc((size_t)v->dec.nblk, sizeof(tts_cnx));
    for (int i = 0; i < v->dec.nblk && v->dec.blk; i++) {
        tts_cnx *b = &v->dec.blk[i];
        b->dw_w = F(v, err, en, "dec.blocks.%d.dw.weight", i); b->dw_b = F(v, err, en, "dec.blocks.%d.dw.bias", i);
        b->norm_w = F(v, err, en, "dec.blocks.%d.norm.weight", i); b->norm_b = F(v, err, en, "dec.blocks.%d.norm.bias", i);
        b->pw0_w = F(v, err, en, "dec.blocks.%d.pw0.weight", i); b->pw0_b = F(v, err, en, "dec.blocks.%d.pw0.bias", i);
        b->pw1_w = F(v, err, en, "dec.blocks.%d.pw1.weight", i); b->pw1_b = F(v, err, en, "dec.blocks.%d.pw1.bias", i);
        b->gamma = F(v, err, en, "dec.blocks.%d.gamma", i);
    }
    v->tilt = F(v, NULL, 0, "tilt");
    if (!v->dur.blk || !v->ac.tblk || !v->ac.fblk || !v->dec.blk) { snprintf(err, en, "out of memory"); return -1; }
    if (err && err[0]) return -1;
    return 0;
}

void tts_net_free(thimble_voice *v) {
    free(v->dur.blk); free(v->ac.tblk); free(v->ac.fblk); free(v->dec.blk);
    v->dur.blk = v->ac.tblk = v->ac.fblk = NULL; v->dec.blk = NULL;
}

/* ------------------------------------------------------------------ kernels */
/* The convolution kernels compute four output channels at a time and split the
 * output channels across threads. Each output accumulates its inputs in the
 * same order either way, so results are bit-identical to a single-threaded
 * run: register blocking alone is about 1.65x here, threads about 3x more. */

typedef struct { const float *W, *b, *x; float *y; int Co, Ci, K, T; } conv_job;

static void conv1_range(void *vj, int o0, int o1) {
    const conv_job *j = vj;
    const float *W = j->W, *b = j->b, *x = j->x;
    float *y = j->y;
    const int Ci = j->Ci, T = j->T;
    int o = o0 * 4;
    const int end = o1 * 4 < j->Co ? o1 * 4 : j->Co;
    for (; o + 4 <= end; o += 4) {
        float *y0 = y + (size_t)o * T, *y1 = y0 + T, *y2 = y1 + T, *y3 = y2 + T;
        const float b0 = b ? b[o] : 0.f, b1 = b ? b[o + 1] : 0.f, b2 = b ? b[o + 2] : 0.f, b3 = b ? b[o + 3] : 0.f;
        for (int t = 0; t < T; t++) { y0[t] = b0; y1[t] = b1; y2[t] = b2; y3[t] = b3; }
        for (int i = 0; i < Ci; i++) {
            const float w0 = W[(size_t)o * Ci + i], w1 = W[(size_t)(o + 1) * Ci + i],
                        w2 = W[(size_t)(o + 2) * Ci + i], w3 = W[(size_t)(o + 3) * Ci + i];
            const float *xi = x + (size_t)i * T;
            for (int t = 0; t < T; t++) { const float v = xi[t]; y0[t] += w0 * v; y1[t] += w1 * v; y2[t] += w2 * v; y3[t] += w3 * v; }
        }
    }
    for (; o < end; o++) {
        float *yo = y + (size_t)o * T;
        const float bo = b ? b[o] : 0.f;
        for (int t = 0; t < T; t++) yo[t] = bo;
        for (int i = 0; i < Ci; i++) {
            const float w = W[(size_t)o * Ci + i];
            const float *xi = x + (size_t)i * T;
            for (int t = 0; t < T; t++) yo[t] += w * xi[t];
        }
    }
}

/* y[o][t] = b[o] + sum_i W[o][i] x[i][t]            (1x1 conv) */
static void conv1(const float *W, const float *b, int Co, int Ci, const float *x, int T, float *y) {
    conv_job j = { W, b, x, y, Co, Ci, 1, T };
    tts_parallel_for((Co + 3) / 4, conv1_range, &j);
}

static void convk_range(void *vj, int o0, int o1) {
    const conv_job *j = vj;
    const float *W = j->W, *b = j->b, *x = j->x;
    float *y = j->y;
    const int Ci = j->Ci, K = j->K, T = j->T, half = j->K / 2;
    int o = o0 * 4;
    const int end = o1 * 4 < j->Co ? o1 * 4 : j->Co;
    for (; o + 4 <= end; o += 4) {
        float *y0 = y + (size_t)o * T, *y1 = y0 + T, *y2 = y1 + T, *y3 = y2 + T;
        const float b0 = b ? b[o] : 0.f, b1 = b ? b[o + 1] : 0.f, b2 = b ? b[o + 2] : 0.f, b3 = b ? b[o + 3] : 0.f;
        for (int t = 0; t < T; t++) { y0[t] = b0; y1[t] = b1; y2[t] = b2; y3[t] = b3; }
        for (int i = 0; i < Ci; i++) {
            const float *xi = x + (size_t)i * T;
            for (int k = 0; k < K; k++) {
                const size_t off_w = (size_t)i * K + k;
                const float w0 = W[(size_t)o * Ci * K + off_w], w1 = W[(size_t)(o + 1) * Ci * K + off_w],
                            w2 = W[(size_t)(o + 2) * Ci * K + off_w], w3 = W[(size_t)(o + 3) * Ci * K + off_w];
                const int off = k - half;
                const int lo = off < 0 ? -off : 0, hi = off > 0 ? T - off : T;
                for (int t = lo; t < hi; t++) {
                    const float v = xi[t + off];
                    y0[t] += w0 * v; y1[t] += w1 * v; y2[t] += w2 * v; y3[t] += w3 * v;
                }
            }
        }
    }
    for (; o < end; o++) {
        float *yo = y + (size_t)o * T;
        const float bo = b ? b[o] : 0.f;
        for (int t = 0; t < T; t++) yo[t] = bo;
        for (int i = 0; i < Ci; i++) {
            const float *xi = x + (size_t)i * T;
            for (int k = 0; k < K; k++) {
                const float w = W[((size_t)o * Ci + i) * K + k];
                const int off = k - half;
                const int lo = off < 0 ? -off : 0, hi = off > 0 ? T - off : T;
                for (int t = lo; t < hi; t++) yo[t] += w * xi[t + off];
            }
        }
    }
}

/* same-padding conv with kernel K */
static void convk(const float *W, const float *b, int Co, int Ci, int K, const float *x, int T, float *y) {
    conv_job j = { W, b, x, y, Co, Ci, K, T };
    tts_parallel_for((Co + 3) / 4, convk_range, &j);
}

typedef struct { const float *W, *b, *x; float *y; int C, K, T; } dw_job;

static void dwconv_range(void *vj, int c0, int c1) {
    const dw_job *j = vj;
    const float *W = j->W, *b = j->b, *x = j->x;
    float *y = j->y;
    const int K = j->K, T = j->T, half = K / 2;
    for (int c = c0; c < c1; c++) {
        float *yc = y + (size_t)c * T; const float *xc = x + (size_t)c * T;
        for (int t = 0; t < T; t++) yc[t] = b[c];
        for (int k = 0; k < K; k++) {
            const float w = W[(size_t)c * K + k];
            const int off = k - half;
            const int lo = off < 0 ? -off : 0, hi = off > 0 ? T - off : T;
            for (int t = lo; t < hi; t++) yc[t] += w * xc[t + off];
        }
    }
}

static void dwconv(const float *W, const float *b, int C, int K, const float *x, int T, float *y) {
    dw_job j = { W, b, x, y, C, K, T };
    tts_parallel_for(C, dwconv_range, &j);
}

/* Activations: elementwise, so the split is just a range of the flat array.
 * Below ACT_PAR elements the fork/join costs more than it saves. */
#define ACT_PAR 16384
static void silu_range(void *x, int a, int b) { float *p = x; for (int i = a; i < b; i++) p[i] = p[i] / (1.f + expf(-p[i])); }
static void gelu_range(void *x, int a, int b) { float *p = x; for (int i = a; i < b; i++) p[i] = 0.5f * p[i] * (1.f + erff(p[i] * 0.70710678118654752f)); }

static void silu_inplace(float *x, size_t n) {
    if (n < ACT_PAR) { silu_range(x, 0, (int)n); return; }
    tts_parallel_for((int)n, silu_range, x);
}

static void gelu_inplace(float *x, size_t n) {
    if (n < ACT_PAR) { gelu_range(x, 0, (int)n); return; }
    tts_parallel_for((int)n, gelu_range, x);
}

/* LayerNorm over the channel axis of [C][T], eps 1e-6. Each frame is
 * independent, so the frames split across threads; the sums over channels keep
 * their order, which keeps the result bit-identical. */
typedef struct { float *x; const float *w, *b; float *mean, *rstd; int C, T; } norm_job;

static void chan_norm_range(void *vj, int t0, int t1) {
    const norm_job *j = vj;
    float *x = j->x, *mean = j->mean, *rstd = j->rstd;
    const int C = j->C, T = j->T;
    for (int t = t0; t < t1; t++) mean[t] = 0.f;
    for (int c = 0; c < C; c++) { const float *xc = x + (size_t)c * T; for (int t = t0; t < t1; t++) mean[t] += xc[t]; }
    for (int t = t0; t < t1; t++) { mean[t] /= (float)C; rstd[t] = 0.f; }
    for (int c = 0; c < C; c++) {
        const float *xc = x + (size_t)c * T;
        for (int t = t0; t < t1; t++) { const float d = xc[t] - mean[t]; rstd[t] += d * d; }
    }
    for (int t = t0; t < t1; t++) rstd[t] = 1.f / sqrtf(rstd[t] / (float)C + 1e-6f);
    for (int c = 0; c < C; c++) {
        float *xc = x + (size_t)c * T; const float wc = j->w[c], bc = j->b[c];
        for (int t = t0; t < t1; t++) xc[t] = (xc[t] - mean[t]) * rstd[t] * wc + bc;
    }
}

static void chan_norm(float *x, int C, int T, const float *w, const float *b, float *tmp) {
    norm_job j = { x, w, b, tmp, tmp + T, C, T };
    tts_parallel_for(T, chan_norm_range, &j);
}

static void linspace01(float *o, int n) {
    if (n <= 1) { if (n == 1) o[0] = 0.f; return; }
    const float step = 1.f / (float)(n - 1);
    for (int i = 0; i < n; i++) o[i] = i < n / 2 ? (float)i * step : 1.f - (float)(n - 1 - i) * step;
}

/* x += scale * conv(silu(conv(x))) over [H][n] */
static void res_block(const tts_res *b, int H, int K, float *x, int n, float *h1, float *h2) {
    convk(b->w0, b->b0, H, H, K, x, n, h1);
    silu_inplace(h1, (size_t)H * n);
    convk(b->w1, b->b1, H, H, K, h1, n, h2);
    for (size_t i = 0; i < (size_t)H * n; i++) x[i] += b->scale * h2[i];
}

/* ------------------------------------------------------------------ duration / acoustic */
static float *dur_log(const thimble_voice *v, const int *ids, int n) {
    const tts_dur *d = &v->dur;
    const int H = d->H;
    float *x = malloc(sizeof(float) * (size_t)(H + 3) * n), *y = malloc(sizeof(float) * (size_t)H * n);
    float *h1 = malloc(sizeof(float) * (size_t)H * n), *h2 = malloc(sizeof(float) * (size_t)H * n);
    float *logd = malloc(sizeof(float) * (size_t)n), *pos = malloc(sizeof(float) * (size_t)n);
    if (!x || !y || !h1 || !h2 || !logd || !pos) { free(x); free(y); free(h1); free(h2); free(logd); free(pos); return NULL; }
    for (int h = 0; h < H; h++) for (int t = 0; t < n; t++) x[(size_t)h * n + t] = d->embed[(size_t)ids[t] * H + h];
    linspace01(pos, n);
    const float hint = log1pf((float)(n > 1 ? n : 1)) / log1pf(207.f);
    for (int t = 0; t < n; t++) { x[(size_t)H * n + t] = pos[t]; x[(size_t)(H + 1) * n + t] = hint; x[(size_t)(H + 2) * n + t] = 1.f; }
    conv1(d->proj_w, d->proj_b, H, H + 3, x, n, y);
    for (int i = 0; i < d->depth; i++) res_block(&d->blk[i], H, 5, y, n, h1, h2);
    conv1(d->out_w, d->out_b, 1, H, y, n, logd);
    free(x); free(y); free(h1); free(h2); free(pos);
    return logd;
}

int tts_durations(const thimble_voice *v, const int *ids, int n, int *durs) {
    float *logd = dur_log(v, ids, n);
    if (!logd) return -1;
    for (int i = 0; i < n; i++) {
        float d = rintf(expf(logd[i]));
        durs[i] = d < 1.f ? 1 : (int)d;
    }
    free(logd);
    return 0;
}

static float *acoustic(const thimble_voice *v, const int *ids, const int *durs, int n, int *T_out) {
    const tts_ac *a = &v->ac;
    const int H = a->H;
    int T = 0, dmax = 1;
    for (int i = 0; i < n; i++) { T += durs[i]; if (durs[i] > dmax) dmax = durs[i]; }
    float *x = malloc(sizeof(float) * (size_t)(H + 2) * n), *y = malloc(sizeof(float) * (size_t)H * n);
    float *h1 = malloc(sizeof(float) * (size_t)H * n), *h2 = malloc(sizeof(float) * (size_t)H * n);
    float *tpos = malloc(sizeof(float) * (size_t)n);
    if (!x || !y || !h1 || !h2 || !tpos) { free(x); free(y); free(h1); free(h2); free(tpos); return NULL; }
    for (int h = 0; h < H; h++) for (int t = 0; t < n; t++) x[(size_t)h * n + t] = a->embed[(size_t)ids[t] * H + h];
    linspace01(tpos, n);
    const float ldm = log1pf((float)dmax);
    for (int t = 0; t < n; t++) { x[(size_t)H * n + t] = tpos[t]; x[(size_t)(H + 1) * n + t] = log1pf((float)durs[t]) / ldm; }
    conv1(a->tproj_w, a->tproj_b, H, H + 2, x, n, y);
    for (int i = 0; i < a->token_depth; i++) res_block(&a->tblk[i], H, 5, y, n, h1, h2);
    free(x); free(h1); free(h2);

    /* repeat by durations and add the three frame-level position features */
    float *fx = malloc(sizeof(float) * (size_t)(H + 3) * T), *fy = malloc(sizeof(float) * (size_t)H * T);
    float *g1 = malloc(sizeof(float) * (size_t)H * T), *g2 = malloc(sizeof(float) * (size_t)H * T);
    float *fpos = malloc(sizeof(float) * (size_t)T), *dpos = malloc(sizeof(float) * (size_t)(dmax + 1));
    float *mel = malloc(sizeof(float) * (size_t)a->n_mels * T);
    if (!fx || !fy || !g1 || !g2 || !fpos || !dpos || !mel) {
        free(fx); free(fy); free(g1); free(g2); free(fpos); free(dpos); free(mel); free(y); free(tpos); return NULL;
    }
    linspace01(fpos, T);
    int t0 = 0;
    for (int i = 0; i < n; i++) {
        linspace01(dpos, durs[i]);
        for (int k = 0; k < durs[i]; k++) {
            const int t = t0 + k;
            for (int h = 0; h < H; h++) fx[(size_t)h * T + t] = y[(size_t)h * n + i];
            fx[(size_t)H * T + t] = fpos[t];
            fx[(size_t)(H + 1) * T + t] = tpos[i];
            fx[(size_t)(H + 2) * T + t] = durs[i] > 1 ? dpos[k] : 0.f;
        }
        t0 += durs[i];
    }
    conv1(a->fproj_w, a->fproj_b, H, H + 3, fx, T, fy);
    for (int i = 0; i < a->depth; i++) res_block(&a->fblk[i], H, 5, fy, T, g1, g2);
    conv1(a->out_w, a->out_b, a->n_mels, H, fy, T, mel);
    free(fx); free(fy); free(g1); free(g2); free(fpos); free(dpos); free(y); free(tpos);
    *T_out = T;
    return mel;
}

/* ------------------------------------------------------------------ decoder + iSTFT */
#define NFFT 1024
#define HOP 256

static void fft_inplace(double *re, double *im, const double *cs, const double *sn, const int *rev, int inverse) {
    for (int i = 0; i < NFFT; i++) {
        int j = rev[i];
        if (j > i) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        const int half = len >> 1, step = NFFT / len;
        for (int i = 0; i < NFFT; i += len) {
            for (int k = 0; k < half; k++) {
                const double wr = cs[k * step], wi = inverse ? sn[k * step] : -sn[k * step];
                const int a = i + k, b = a + half;
                const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
        }
    }
}

/* One iSTFT frame: spectrum -> windowed time-domain samples, written to its own
 * slot of the block buffer (no two frames share a slot). */
typedef struct {
    const float *head; double *fbuf;
    const double *cs, *sn, *win; const int *rev;
    int bins, T, t0;
} frame_job;

static void frame_range(void *vj, int k0, int k1) {
    const frame_job *j = vj;
    const int BINS = j->bins, T = j->T;
    double re[NFFT], im[NFFT];
    for (int k = k0; k < k1; k++) {
        const int t = j->t0 + k;
        for (int bi = 0; bi < BINS; bi++) {
            float mag = (bi == 0 || bi == BINS - 1) ? 0.f : expf(j->head[(size_t)bi * T + t]);
            if (mag > 1e2f) mag = 1e2f;
            const float ph = j->head[(size_t)(BINS + bi) * T + t];
            const double r = (double)mag * cosf(ph), i = (double)mag * sinf(ph);
            re[bi] = r; im[bi] = i;
            if (bi > 0 && bi < NFFT / 2) { re[NFFT - bi] = r; im[NFFT - bi] = -i; }
        }
        im[0] = 0; im[NFFT / 2] = 0;
        fft_inplace(re, im, j->cs, j->sn, j->rev, 1);
        double *f = j->fbuf + (size_t)k * NFFT;
        for (int i = 0; i < NFFT; i++) f[i] = re[i] / NFFT * j->win[i];
    }
}

int tts_synth_ids(const thimble_voice *v, const int *ids, int n, float **wav_out) {
    *wav_out = NULL;
    int *durs = malloc(sizeof(int) * (size_t)n);
    if (!durs || tts_durations(v, ids, n, durs)) { free(durs); return -1; }
    int T = 0;
    float *mel = acoustic(v, ids, durs, n, &T);
    free(durs);
    if (!mel) return -1;
    const int Tfull = T;
    if (T > 2400) T = 2400;     /* training cap; callers chunk well below it */
    const tts_dec *d = &v->dec;
    const int C = d->dim, M = d->n_mels, BINS = d->bins;
    float *m2 = mel;
    if (Tfull != T) {
        m2 = malloc(sizeof(float) * (size_t)M * T);
        for (int c = 0; c < M; c++) memcpy(m2 + (size_t)c * T, mel + (size_t)c * Tfull, sizeof(float) * (size_t)T);
        free(mel);
    }
    if (v->tilt) for (int c = 0; c < M; c++) for (int t = 0; t < T; t++) m2[(size_t)c * T + t] += v->tilt[c];

    float *x = malloc(sizeof(float) * (size_t)C * T), *h = malloc(sizeof(float) * (size_t)C * T);
    float *p = malloc(sizeof(float) * (size_t)d->pw * T), *q = malloc(sizeof(float) * (size_t)C * T);
    float *tmp = malloc(sizeof(float) * 2 * (size_t)T);
    float *head = malloc(sizeof(float) * (size_t)BINS * 2 * T);
    if (!x || !h || !p || !q || !tmp || !head) { free(m2); free(x); free(h); free(p); free(q); free(tmp); free(head); return -1; }
    convk(d->embed_w, d->embed_b, C, M, 7, m2, T, x);
    free(m2);
    for (int c = 0; c < C; c++) for (int t = 0; t < T; t++) x[(size_t)c * T + t] += d->noise_b[c];   /* zero noise: conv is its bias */
    chan_norm(x, C, T, d->norm_w, d->norm_b, tmp);
    for (int bi = 0; bi < d->nblk; bi++) {
        const tts_cnx *b = &d->blk[bi];
        dwconv(b->dw_w, b->dw_b, C, 7, x, T, h);
        chan_norm(h, C, T, b->norm_w, b->norm_b, tmp);
        conv1(b->pw0_w, b->pw0_b, d->pw, C, h, T, p);
        gelu_inplace(p, (size_t)d->pw * T);
        conv1(b->pw1_w, b->pw1_b, C, d->pw, p, T, q);
        for (int c = 0; c < C; c++) { const float g = b->gamma[c]; for (int t = 0; t < T; t++) x[(size_t)c * T + t] += q[(size_t)c * T + t] * g; }
    }
    chan_norm(x, C, T, d->fnorm_w, d->fnorm_b, tmp);
    conv1(d->head_w, d->head_b, BINS * 2, C, x, T, head);
    free(x); free(h); free(p); free(q); free(tmp);

    /* iSTFT: Hann-windowed overlap-add, exactly thimble/tts/models.py::istft.
     * The frames of a block are transformed in parallel into a scratch buffer,
     * then overlap-added in order: the sum each output sample receives keeps
     * its frame order, so the waveform does not depend on the thread count. */
    static double cs[NFFT], sn[NFFT], win[NFFT]; static int rev[NFFT]; static int ready = 0;
    if (!ready) {
        for (int i = 0; i < NFFT; i++) {
            cs[i] = cos(2.0 * M_PI * i / NFFT); sn[i] = sin(2.0 * M_PI * i / NFFT);
            win[i] = 0.5 - 0.5 * cos(2.0 * M_PI * i / NFFT);
            int r = 0; for (int bi = 0, x2 = i; bi < 10; bi++, x2 >>= 1) r = r << 1 | (x2 & 1);
            rev[i] = r;
        }
        ready = 1;
    }
    const size_t length = (size_t)(T - 1) * HOP + NFFT;
    double *ola = calloc(length, sizeof(double)), *nrm = calloc(length, sizeof(double));
    const int FB = 64;                              /* frames per block: 64 x 1024 doubles = 512 KB */
    double *fbuf = malloc(sizeof(double) * (size_t)FB * NFFT);
    if (!ola || !nrm || !fbuf) { free(ola); free(nrm); free(fbuf); free(head); return -1; }
    frame_job fj = { head, fbuf, cs, sn, win, rev, BINS, T, 0 };
    for (int t0 = 0; t0 < T; t0 += FB) {
        const int nb = T - t0 < FB ? T - t0 : FB;
        fj.t0 = t0;
        tts_parallel_for(nb, frame_range, &fj);
        for (int k = 0; k < nb; k++) {
            const double *f = fbuf + (size_t)k * NFFT;
            double *o = ola + (size_t)(t0 + k) * HOP, *w = nrm + (size_t)(t0 + k) * HOP;
            for (int i = 0; i < NFFT; i++) { o[i] += f[i]; w[i] += win[i] * win[i]; }
        }
    }
    free(fbuf);
    free(head);
    const size_t pad = NFFT / 2, ns = length - 2 * pad;
    float *out = malloc(sizeof(float) * ns);
    if (!out) { free(ola); free(nrm); return -1; }
    for (size_t i = 0; i < ns; i++) {
        double s = ola[i + pad], w = nrm[i + pad];
        if (w > 1e-11) s /= w;
        ola[i + pad] = s;
    }
    /* DC blocker: 4096-tap FIR h[0]=1, h[k]=(r-1) r^(k-1), run as an O(N) recursion */
    const double r = 0.9973, rp = pow(r, 4094.0);
    double sacc = 0.0;
    for (size_t i = 0; i < ns; i++) {
        const double xi = ola[i + pad];
        out[i] = (float)(xi + (r - 1.0) * sacc);
        const double old = i >= 4095 ? ola[i + pad - 4095] : 0.0;
        sacc = xi + r * (sacc - rp * old);
    }
    free(ola); free(nrm);
    *wav_out = out;
    return (int)ns;
}
