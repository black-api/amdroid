/* Inference context and the per-token forward pass. */
#include "model.h"
#include "ops.h"

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int imax(int a, int b) { return a > b ? a : b; }

thimble_ctx *thimble_ctx_new(const thimble_model *m, int n_ctx) {
    const thimble_hparams *hp = &m->hp;
    if (n_ctx <= 0) n_ctx = hp->n_ctx_train;
    if (hp->learned_pos && n_ctx > hp->n_ctx_train) n_ctx = hp->n_ctx_train;

    thimble_ctx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->m = m, c->n_ctx = n_ctx;
    const int d = hp->n_embd, qd = hp->n_head * hp->head_dim;
    const int ff = imax(imax(hp->n_ff, hp->n_ff_exp), d);
    c->state = calloc((size_t) hp->n_layer, sizeof(float *));
    int ok = c->state != NULL;
    for (int il = 0; ok && il < hp->n_layer; il++) {
        const thimble_layer *L = &m->layers[il];
        size_t n = L->mixer->state_floats(m, L, n_ctx);
        c->state[il] = calloc(n ? n : 1, sizeof(float));
        ok = c->state[il] != NULL;
    }
#define ALLOC(p, n) do { if (ok) { (p) = calloc((size_t) (n), sizeof(*(p))); ok = (p) != NULL; } } while (0)
    ALLOC(c->x, d);  ALLOC(c->h, d);  ALLOC(c->y, d);
    ALLOC(c->q, qd); ALLOC(c->k, qd); ALLOC(c->v, qd); ALLOC(c->att, qd); ALLOC(c->gate, qd);
    ALLOC(c->scores, n_ctx);
    ALLOC(c->bcx, 3 * d);
    ALLOC(c->f1, ff); ALLOC(c->f2, ff); ALLOC(c->f3, ff);
    ALLOC(c->probs, imax(hp->n_expert, 1));
    ALLOC(c->sel, imax(hp->n_expert_used, 1));
    ALLOC(c->logits, hp->n_vocab);
    if (hp->copy_head) {
        ALLOC(c->ctx_tokens, n_ctx);
        ALLOC(c->cq, hp->copy_dim);
        ALLOC(c->ckeys, (size_t) n_ctx * hp->copy_dim);
        ALLOC(c->cemb, d);
        ALLOC(c->cscores, n_ctx);
    }
#undef ALLOC
    if (!ok) {
        thimble_ctx_free(c);
        return NULL;
    }
    return c;
}

void thimble_ctx_free(thimble_ctx *c) {
    if (!c) return;
    if (c->state)
        for (int il = 0; il < c->m->hp.n_layer; il++) free(c->state[il]);
    free(c->state);
    free(c->x); free(c->h); free(c->y);
    free(c->q); free(c->k); free(c->v); free(c->att); free(c->gate);
    free(c->scores); free(c->bcx);
    free(c->f1); free(c->f2); free(c->f3);
    free(c->probs); free(c->sel); free(c->logits);
    free(c->ctx_tokens); free(c->cq); free(c->ckeys); free(c->cemb); free(c->cscores);
    free(c);
}

void thimble_ctx_reset(thimble_ctx *c) {
    for (int il = 0; il < c->m->hp.n_layer; il++) {
        const thimble_layer *L = &c->m->layers[il];
        memset(c->state[il], 0, sizeof(float) * L->mixer->state_floats(c->m, L, c->n_ctx));
    }
    c->pos = 0;
}

int thimble_ctx_pos(const thimble_ctx *c) { return c->pos; }
int thimble_ctx_n_ctx(const thimble_ctx *c) { return c->n_ctx; }

int thimble_ctx_truncate(thimble_ctx *c, int n_keep) {
    if (n_keep < 0 || n_keep > c->pos) return -1;
    c->pos = n_keep;   /* every layer keeps per-position state; later entries are overwritten */
    return 0;
}

int thimble_ctx_copy(thimble_ctx *dst, const thimble_ctx *src) {
    if (dst->m != src->m || dst->n_ctx < src->pos) return -1;
    if (src->ctx_tokens && dst->ctx_tokens) {
        memcpy(dst->ctx_tokens, src->ctx_tokens, sizeof(int) * (size_t) src->pos);
        const int cd = src->m->hp.copy_dim;
        memcpy(dst->ckeys, src->ckeys, sizeof(float) * (size_t) src->pos * cd);
    }
    for (int il = 0; il < src->m->hp.n_layer; il++) {
        const thimble_layer *L = &src->m->layers[il];
        /* per-position state: copy only the positions in use (K/V, conv history) */
        size_t full = L->mixer->state_floats(src->m, L, src->n_ctx);
        size_t per  = full / (size_t) src->n_ctx;
        if (L->mixer == &MIXER_ATTENTION) {   /* [K: n_ctx x kd][V: n_ctx x kd] */
            size_t half = per / 2, used = (size_t) src->pos * half;
            memcpy(dst->state[il], src->state[il], sizeof(float) * used);
            memcpy(dst->state[il] + (size_t) dst->n_ctx * half, src->state[il] + (size_t) src->n_ctx * half,
                   sizeof(float) * used);
        } else {
            memcpy(dst->state[il], src->state[il], sizeof(float) * (size_t) src->pos * per);
        }
    }
    dst->pos = src->pos;
    return 0;
}

/* Pointer/copy head: mix an attention over the tokens seen so far into the
 * output distribution, then return log p so that the sampler's temperature,
 * top-k and top-p keep working unchanged -- exactly as the PyTorch side does.
 *
 *   p(v) = g * softmax(logits)[v] + (1 - g) * sum_j attn[j] * [ctx[j] == v]
 */
static int token_at(const thimble_ctx *c, int pos) { return c->ctx_tokens[pos]; }

static void copy_head_apply(thimble_ctx *c, const float *h) {
    const thimble_model   *m  = c->m;
    const thimble_hparams *hp = &m->hp;
    const int d = hp->n_embd, cd = hp->copy_dim, n = c->pos + 1;   /* includes this token */

    op_matvec(c->cq, m->copy_q, h, cd, d);

    /* A pointer key depends only on its token, so it is computed once when the
     * position is first seen and reused for the rest of the sequence. Without
     * this the head re-projects the whole context every step, which cost about
     * 17% of decode throughput. */
    float *kcur = c->ckeys + (size_t) c->pos * cd;
    dequantize_row(m->tok_embd.type,
                   (const uint8_t *) m->tok_embd.data + (size_t) token_at(c, c->pos) * m->tok_embd.row_bytes,
                   c->cemb, d);
    op_matvec(kcur, m->copy_k, c->cemb, cd, d);

    float best = -INFINITY;
    const float scale = 1.0f / sqrtf((float) cd);
    for (int j = 0; j < n; j++) {
        const float *kj = c->ckeys + (size_t) j * cd;
        float dot = 0.0f;
        for (int i = 0; i < cd; i++) dot += c->cq[i] * kj[i];
        c->cscores[j] = dot * scale;
        if (c->cscores[j] > best) best = c->cscores[j];
    }
    float sum = 0.0f;
    for (int j = 0; j < n; j++) sum += (c->cscores[j] = expf(c->cscores[j] - best));
    for (int j = 0; j < n; j++) c->cscores[j] /= sum;

    float gate = m->copy_gate_b[0];
    for (int i = 0; i < d; i++) gate += m->copy_gate_w[i] * h[i];
    const float g = 1.0f / (1.0f + expf(-gate));

    /* p_vocab, scaled by the gate */
    float lmax = -INFINITY;
    for (int v = 0; v < hp->n_vocab; v++) if (c->logits[v] > lmax) lmax = c->logits[v];
    float lsum = 0.0f;
    for (int v = 0; v < hp->n_vocab; v++) lsum += (c->logits[v] = expf(c->logits[v] - lmax));
    for (int v = 0; v < hp->n_vocab; v++) c->logits[v] *= g / lsum;

    /* add the pointer mass onto the ids it points at */
    for (int j = 0; j < n; j++) c->logits[c->ctx_tokens[j]] += (1.0f - g) * c->cscores[j];

    for (int v = 0; v < hp->n_vocab; v++) {
        float p = c->logits[v];
        c->logits[v] = logf(p < 1e-9f ? 1e-9f : p);
    }
}

const float *thimble_eval(thimble_ctx *c, int token) {
    const thimble_model   *m  = c->m;
    const thimble_hparams *hp = &m->hp;
    const int d = hp->n_embd;
    if (c->pos >= c->n_ctx || token < 0 || token >= hp->n_vocab) return NULL;
    if (c->ctx_tokens) c->ctx_tokens[c->pos] = token;

    dequantize_row(m->tok_embd.type, (const uint8_t *) m->tok_embd.data + (size_t) token * m->tok_embd.row_bytes,
                   c->x, d);
    if (m->pos_embd) op_add(c->x, m->pos_embd + (size_t) c->pos * d, d);

    for (int il = 0; il < hp->n_layer; il++) {
        const thimble_layer *L = &m->layers[il];
        model_norm(m, c->h, c->x, L->attn_norm, L->attn_norm_b);
        L->mixer->forward(c, L, c->state[il], c->h, c->y);
        op_add(c->x, c->y, d);
        model_norm(m, c->h, c->x, L->ffn_norm, L->ffn_norm_b);
        L->ffn->forward(c, L, c->h, c->y);
        op_add(c->x, c->y, d);
    }
    model_norm(m, c->h, c->x, m->out_norm, m->out_norm_b);
    op_matvec_w(c->logits, &m->output, 0, c->h, hp->n_vocab, d);
    if (hp->copy_head) copy_head_apply(c, c->h);
    c->pos++;
    return c->logits;
}
