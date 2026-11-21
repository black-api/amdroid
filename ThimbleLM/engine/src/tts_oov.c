/* Grapheme -> phoneme fallback for words the dictionary lacks: a 1-head, 1+1 layer
 * BART, greedy decoding. Mirrors thimble/tts/g2p/oov.py (post-norm, erf GELU with
 * the same Abramowitz-Stegun polynomial, learned positions with offset 2). */
#include "tts.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const float *T(const thimble_voice *v, char *err, size_t n, const char *fmt, ...) {
    char name[200];
    va_list ap; va_start(ap, fmt); vsnprintf(name, sizeof name, fmt, ap); va_end(ap);
    const tts_tensor *t = tts_file_find(&v->file, name);
    if (!t || t->dtype != TT_F32) { if (err && n && !err[0]) snprintf(err, n, "voice file is missing tensor %s", name); return NULL; }
    return (const float *)tts_file_data(&v->file, t);
}

static void load_attn(const thimble_voice *v, char *err, size_t n, tts_attn *a, const char *pre) {
    a->q_w = T(v, err, n, "oov.%s.q_proj.weight", pre); a->q_b = T(v, err, n, "oov.%s.q_proj.bias", pre);
    a->k_w = T(v, err, n, "oov.%s.k_proj.weight", pre); a->k_b = T(v, err, n, "oov.%s.k_proj.bias", pre);
    a->v_w = T(v, err, n, "oov.%s.v_proj.weight", pre); a->v_b = T(v, err, n, "oov.%s.v_proj.bias", pre);
    a->o_w = T(v, err, n, "oov.%s.out_proj.weight", pre); a->o_b = T(v, err, n, "oov.%s.out_proj.bias", pre);
}

static void load_layer(const thimble_voice *v, char *err, size_t n, tts_layer *l, const char *pre, int cross) {
    char b[160];
    snprintf(b, sizeof b, "%s.self_attn", pre); load_attn(v, err, n, &l->self, b);
    l->self_ln_w = T(v, err, n, "oov.%s.self_attn_layer_norm.weight", pre);
    l->self_ln_b = T(v, err, n, "oov.%s.self_attn_layer_norm.bias", pre);
    if (cross) {
        snprintf(b, sizeof b, "%s.encoder_attn", pre); load_attn(v, err, n, &l->cross, b);
        l->cross_ln_w = T(v, err, n, "oov.%s.encoder_attn_layer_norm.weight", pre);
        l->cross_ln_b = T(v, err, n, "oov.%s.encoder_attn_layer_norm.bias", pre);
    }
    l->fc1_w = T(v, err, n, "oov.%s.fc1.weight", pre); l->fc1_b = T(v, err, n, "oov.%s.fc1.bias", pre);
    l->fc2_w = T(v, err, n, "oov.%s.fc2.weight", pre); l->fc2_b = T(v, err, n, "oov.%s.fc2.bias", pre);
    l->final_ln_w = T(v, err, n, "oov.%s.final_layer_norm.weight", pre);
    l->final_ln_b = T(v, err, n, "oov.%s.final_layer_norm.bias", pre);
}

int tts_oov_init(thimble_voice *v, char *err, size_t en) {
    tts_oov *o = &v->oov;
    const char *gc = tts_file_meta(&v->file, "oov_grapheme_chars"), *pc = tts_file_meta(&v->file, "oov_phoneme_chars");
    if (!gc || !pc) { snprintf(err, en, "voice file has no fallback model"); return -1; }
    o->max_pos = atoi(tts_file_meta(&v->file, "oov_max_positions") ? tts_file_meta(&v->file, "oov_max_positions") : "64");
    o->bos = atoi(tts_file_meta(&v->file, "oov_bos") ? tts_file_meta(&v->file, "oov_bos") : "1");
    o->eos = atoi(tts_file_meta(&v->file, "oov_eos") ? tts_file_meta(&v->file, "oov_eos") : "2");
    o->start = atoi(tts_file_meta(&v->file, "oov_start") ? tts_file_meta(&v->file, "oov_start") : "2");
    o->offset = atoi(tts_file_meta(&v->file, "oov_offset") ? tts_file_meta(&v->file, "oov_offset") : "2");
    o->eps = (float)atof(tts_file_meta(&v->file, "oov_eps") ? tts_file_meta(&v->file, "oov_eps") : "1e-5");
    for (int i = 0; i < 256; i++) o->g2id[i] = -1;
    for (int i = 4; gc[i]; i++) o->g2id[(unsigned char)gc[i]] = i;
    o->n_ph = 0;
    for (size_t i = 0, adv; pc[i] && o->n_ph < 128; i += adv) {
        o->ph_chars[o->n_ph++] = (unsigned char)tts_decode_utf8((const unsigned char *)pc + i, strlen(pc + i), &adv);
    }
    const tts_tensor *sh = tts_file_find(&v->file, "oov.model.shared.weight");
    if (!sh) { snprintf(err, en, "voice file has no fallback model"); return -1; }
    o->n_tokens = sh->shape[0]; o->d = sh->shape[1];
    const tts_tensor *f1 = tts_file_find(&v->file, "oov.model.encoder.layers.0.fc1.weight");
    o->ff = f1 ? f1->shape[0] : 1024;
    if (err) err[0] = 0;
    o->embed = T(v, err, en, "oov.model.shared.weight");
    const float *lb = T(v, err, en, "oov.final_logits_bias"); o->logit_bias = lb;
    o->enc_pos = T(v, err, en, "oov.model.encoder.embed_positions.weight");
    o->dec_pos = T(v, err, en, "oov.model.decoder.embed_positions.weight");
    o->enc_ln_w = T(v, err, en, "oov.model.encoder.layernorm_embedding.weight");
    o->enc_ln_b = T(v, err, en, "oov.model.encoder.layernorm_embedding.bias");
    o->dec_ln_w = T(v, err, en, "oov.model.decoder.layernorm_embedding.weight");
    o->dec_ln_b = T(v, err, en, "oov.model.decoder.layernorm_embedding.bias");
    load_layer(v, err, en, &o->enc, "model.encoder.layers.0", 0);
    load_layer(v, err, en, &o->dec, "model.decoder.layers.0", 1);
    return err && err[0] ? -1 : 0;
}

/* ---- small dense helpers: row-major, y[o] = b[o] + sum_i W[o][i] x[i] ---- */
static void lin(const float *W, const float *b, int out, int in, const float *x, float *y) {
    for (int o = 0; o < out; o++) {
        const float *w = W + (size_t)o * in; float s = b ? b[o] : 0.f;
        for (int i = 0; i < in; i++) s += w[i] * x[i];
        y[o] = s;
    }
}

static void layer_norm(float *x, int d, const float *w, const float *b, float eps) {
    float mean = 0, var = 0;
    for (int i = 0; i < d; i++) mean += x[i];
    mean /= (float)d;
    for (int i = 0; i < d; i++) { float t = x[i] - mean; var += t * t; }
    var /= (float)d;
    const float inv = 1.f / sqrtf(var + eps);
    for (int i = 0; i < d; i++) x[i] = (x[i] - mean) * inv * w[i] + b[i];
}

static float gelu_as(float x) {   /* A&S 7.1.26 erf, as the Python port */
    float v = x / 1.41421356237f, sign = v < 0 ? -1.f : 1.f, ax = fabsf(v);
    float t = 1.f / (1.f + 0.3275911f * ax);
    float poly = t * (0.254829592f + t * (-0.284496736f + t * (1.421413741f + t * (-1.453152027f + t * 1.061405429f))));
    float erf = sign * (1.f - poly * expf(-ax * ax));
    return 0.5f * x * (1.f + erf);
}

/* one query row against K/V rows [L][d]; causal handled by passing only visible rows */
static void attend(const tts_attn *a, int d, const float *q_in, const float *K, const float *V, int L, float *out, float *scratch) {
    float *q = scratch, *sc = scratch + d, *ctx = scratch + d + L, *tmp = scratch + 2 * d + L;
    lin(a->q_w, a->q_b, d, d, q_in, q);
    const float scaling = 1.f / sqrtf((float)d);
    for (int i = 0; i < d; i++) q[i] *= scaling;
    float mx = -1e30f;
    for (int j = 0; j < L; j++) {
        float s = 0; const float *k = K + (size_t)j * d;
        for (int i = 0; i < d; i++) s += q[i] * k[i];
        sc[j] = s; if (s > mx) mx = s;
    }
    float sum = 0;
    for (int j = 0; j < L; j++) { sc[j] = expf(sc[j] - mx); sum += sc[j]; }
    for (int i = 0; i < d; i++) ctx[i] = 0;
    for (int j = 0; j < L; j++) { const float w = sc[j] / sum; const float *vv = V + (size_t)j * d; for (int i = 0; i < d; i++) ctx[i] += w * vv[i]; }
    lin(a->o_w, a->o_b, d, d, ctx, tmp);
    memcpy(out, tmp, sizeof(float) * (size_t)d);
}

int tts_oov_word(const thimble_voice *v, const unsigned char *word, int len, unsigned char *out, int cap) {
    const tts_oov *o = &v->oov;
    const int d = o->d, ff = o->ff;
    int ids[130], n = 0;
    ids[n++] = o->bos;
    for (int i = 0; i < len && n < 128; i++) { int g = o->g2id[word[i]]; ids[n++] = g < 0 ? 3 : g; }
    ids[n++] = o->eos;
    if (n > o->max_pos || len <= 0) return -1;

    float *mem = malloc(sizeof(float) * (size_t)n * d), *Km = malloc(sizeof(float) * (size_t)n * d), *Vm = malloc(sizeof(float) * (size_t)n * d);
    float *scr = malloc(sizeof(float) * (size_t)(4 * d + 2 * o->max_pos + 8)), *hid = malloc(sizeof(float) * (size_t)d * 4);
    float *ffb = malloc(sizeof(float) * (size_t)ff), *Ks = malloc(sizeof(float) * (size_t)o->max_pos * d), *Vs = malloc(sizeof(float) * (size_t)o->max_pos * d);
    float *Hs = malloc(sizeof(float) * (size_t)o->max_pos * d), *logits = malloc(sizeof(float) * (size_t)o->n_tokens);
    if (!mem || !Km || !Vm || !scr || !hid || !ffb || !Ks || !Vs || !Hs || !logits) { free(mem); free(Km); free(Vm); free(scr); free(hid); free(ffb); free(Ks); free(Vs); free(Hs); free(logits); return -1; }

    /* ---- encoder (one layer, no mask) ---- */
    float *E = malloc(sizeof(float) * (size_t)n * d);
    for (int t = 0; t < n; t++) {
        float *h = E + (size_t)t * d;
        for (int i = 0; i < d; i++) h[i] = o->embed[(size_t)ids[t] * d + i] + o->enc_pos[(size_t)(o->offset + t) * d + i];
        layer_norm(h, d, o->enc_ln_w, o->enc_ln_b, o->eps);
    }
    for (int t = 0; t < n; t++) {
        lin(o->enc.self.k_w, o->enc.self.k_b, d, d, E + (size_t)t * d, Km + (size_t)t * d);
        lin(o->enc.self.v_w, o->enc.self.v_b, d, d, E + (size_t)t * d, Vm + (size_t)t * d);
    }
    for (int t = 0; t < n; t++) {
        float *h = hid;
        attend(&o->enc.self, d, E + (size_t)t * d, Km, Vm, n, h, scr);
        for (int i = 0; i < d; i++) h[i] += E[(size_t)t * d + i];
        layer_norm(h, d, o->enc.self_ln_w, o->enc.self_ln_b, o->eps);
        lin(o->enc.fc1_w, o->enc.fc1_b, ff, d, h, ffb);
        for (int i = 0; i < ff; i++) ffb[i] = gelu_as(ffb[i]);
        float *f2 = hid + d; lin(o->enc.fc2_w, o->enc.fc2_b, d, ff, ffb, f2);
        for (int i = 0; i < d; i++) f2[i] += h[i];
        layer_norm(f2, d, o->enc.final_ln_w, o->enc.final_ln_b, o->eps);
        memcpy(mem + (size_t)t * d, f2, sizeof(float) * (size_t)d);
    }
    free(E);
    /* cross-attention keys/values from the encoder output */
    for (int t = 0; t < n; t++) {
        lin(o->dec.cross.k_w, o->dec.cross.k_b, d, d, mem + (size_t)t * d, Km + (size_t)t * d);
        lin(o->dec.cross.v_w, o->dec.cross.v_b, d, d, mem + (size_t)t * d, Vm + (size_t)t * d);
    }

    /* ---- greedy decoding; only the last position's output is needed ---- */
    int dec_ids[130], nd = 0, nout = 0;
    dec_ids[nd++] = o->start;
    for (int step = 0; step < o->max_pos - 1; step++) {
        for (int t = 0; t < nd; t++) {         /* layer inputs of every prefix position */
            float *h = Hs + (size_t)t * d;
            for (int i = 0; i < d; i++) h[i] = o->embed[(size_t)dec_ids[t] * d + i] + o->dec_pos[(size_t)(o->offset + t) * d + i];
            layer_norm(h, d, o->dec_ln_w, o->dec_ln_b, o->eps);
            lin(o->dec.self.k_w, o->dec.self.k_b, d, d, h, Ks + (size_t)t * d);
            lin(o->dec.self.v_w, o->dec.self.v_b, d, d, h, Vs + (size_t)t * d);
        }
        const float *last = Hs + (size_t)(nd - 1) * d;
        float *h = hid;
        attend(&o->dec.self, d, last, Ks, Vs, nd, h, scr);
        for (int i = 0; i < d; i++) h[i] += last[i];
        layer_norm(h, d, o->dec.self_ln_w, o->dec.self_ln_b, o->eps);
        float *c = hid + d;
        attend(&o->dec.cross, d, h, Km, Vm, n, c, scr);
        for (int i = 0; i < d; i++) c[i] += h[i];
        layer_norm(c, d, o->dec.cross_ln_w, o->dec.cross_ln_b, o->eps);
        lin(o->dec.fc1_w, o->dec.fc1_b, ff, d, c, ffb);
        for (int i = 0; i < ff; i++) ffb[i] = gelu_as(ffb[i]);
        float *f2 = hid + 2 * d; lin(o->dec.fc2_w, o->dec.fc2_b, d, ff, ffb, f2);
        for (int i = 0; i < d; i++) f2[i] += c[i];
        layer_norm(f2, d, o->dec.final_ln_w, o->dec.final_ln_b, o->eps);
        int best = 0; float bv = -1e30f;
        for (int k = 0; k < o->n_tokens; k++) {
            float s = o->logit_bias[k]; const float *e = o->embed + (size_t)k * d;
            for (int i = 0; i < d; i++) s += f2[i] * e[i];
            logits[k] = s; if (s > bv) { bv = s; best = k; }
        }
        if (best == o->eos) break;
        dec_ids[nd++] = best;
        if (best >= 4 && best < o->n_ph && nout < cap) out[nout++] = o->ph_chars[best];
        if (nd >= 129) break;
    }
    free(mem); free(Km); free(Vm); free(scr); free(hid); free(ffb); free(Ks); free(Vs); free(Hs); free(logits);
    return nout;
}
