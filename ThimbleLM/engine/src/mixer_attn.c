/* Causal self-attention with KV cache.
 *   GQA (n_kv_head <= n_head), optional per-head QK-RMSNorm (v2/v4),
 *   RoPE unless the model uses learned positions, optional v4 output gate:
 *   out = Wo (SDPA(q, k, v) * sigmoid(Wgate x)). */
#include "model.h"
#include "ops.h"

#include <math.h>
#include <string.h>

static int attn_load(thimble_model *m, thimble_layer *L, int il) {
    const int d  = m->hp.n_embd, hd = m->hp.head_dim;
    const int qd = m->hp.n_head * hd, kd = L->n_kv_head * hd;
    if (L->n_kv_head <= 0 || m->hp.n_head % L->n_kv_head)
        return model_error(m, "layer %d: n_head %d not divisible by n_kv_head %d", il, m->hp.n_head,
                           L->n_kv_head);
    L->wq     = model_weight(m, il, "attn_q.weight", 1, d, qd, -1);
    L->wk     = model_weight(m, il, "attn_k.weight", 1, d, kd, -1);
    L->wv     = model_weight(m, il, "attn_v.weight", 1, d, kd, -1);
    L->wo     = model_weight(m, il, "attn_output.weight", 1, qd, d, -1);
    L->wgate  = model_weight(m, il, "attn_gate.weight", 0, d, qd, -1);
    L->q_norm = model_tensor(m, il, "attn_q_norm.weight", 0, hd, -1, -1);
    L->k_norm = model_tensor(m, il, "attn_k_norm.weight", 0, hd, -1, -1);
    if (!L->wq.data || !L->wk.data || !L->wv.data || !L->wo.data) return -1;
    if (!L->q_norm != !L->k_norm) return model_error(m, "layer %d: q_norm/k_norm must come together", il);
    return 0;
}

static size_t attn_state(const thimble_model *m, const thimble_layer *L, int n_ctx) {
    return 2 * (size_t) n_ctx * (size_t) (L->n_kv_head * m->hp.head_dim);   /* K and V */
}

static void attn_forward(thimble_ctx *c, const thimble_layer *L, float *state, const float *in,
                         float *out) {
    const thimble_hparams *hp = &c->m->hp;
    const int H = hp->n_head, KV = L->n_kv_head, hd = hp->head_dim, pos = c->pos;
    const int qd = H * hd, kd = KV * hd, group = H / KV;
    float *kcache = state, *vcache = state + (size_t) c->n_ctx * kd;

    op_matvec_w(c->q, &L->wq, 0, in, qd, hp->n_embd);
    op_matvec_w(c->k, &L->wk, 0, in, kd, hp->n_embd);
    op_matvec_w(c->v, &L->wv, 0, in, kd, hp->n_embd);
    if (L->q_norm) {
        for (int h = 0; h < H; h++)  op_rmsnorm(c->q + h * hd, c->q + h * hd, L->q_norm, hd, hp->rms_eps);
        for (int h = 0; h < KV; h++) op_rmsnorm(c->k + h * hd, c->k + h * hd, L->k_norm, hd, hp->rms_eps);
    }
    if (!hp->learned_pos) {
        op_rope(c->q, H, hd, pos, hp->rope_base);
        op_rope(c->k, KV, hd, pos, hp->rope_base);
    }
    memcpy(kcache + (size_t) pos * kd, c->k, sizeof(float) * (size_t) kd);
    memcpy(vcache + (size_t) pos * kd, c->v, sizeof(float) * (size_t) kd);

    const float scale = 1.0f / sqrtf((float) hd);
    for (int h = 0; h < H; h++) {
        const float *q   = c->q + h * hd;
        const int    kvh = h / group;
        for (int t = 0; t <= pos; t++)
            c->scores[t] = op_dot(q, kcache + (size_t) t * kd + kvh * hd, hd) * scale;
        op_softmax(c->scores, pos + 1);
        float *o = c->att + h * hd;
        memset(o, 0, sizeof(float) * (size_t) hd);
        for (int t = 0; t <= pos; t++) op_axpy(o, c->scores[t], vcache + (size_t) t * kd + kvh * hd, hd);
    }
    if (L->wgate.data) {
        op_matvec_w(c->gate, &L->wgate, 0, in, qd, hp->n_embd);
        for (int i = 0; i < qd; i++) c->att[i] *= op_sigmoid(c->gate[i]);
    }
    op_matvec_w(out, &L->wo, 0, c->att, hp->n_embd, qd);
}

const thimble_mixer MIXER_ATTENTION = { "attention", attn_load, attn_state, attn_forward };
