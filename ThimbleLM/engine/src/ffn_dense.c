/* Dense FFN: SwiGLU (ffn_gate present) or GELU(erf) (thimble-v1/v2 default). */
#include "model.h"
#include "ops.h"

static int dense_load(thimble_model *m, thimble_layer *L, int il) {
    const int d = m->hp.n_embd, ff = m->hp.n_ff;
    L->ffn_up   = model_weight(m, il, "ffn_up.weight", 1, d, ff, -1);
    L->ffn_gate = model_weight(m, il, "ffn_gate.weight", 0, d, ff, -1);
    L->ffn_down = model_weight(m, il, "ffn_down.weight", 1, ff, d, -1);
    return (L->ffn_up.data && L->ffn_down.data) ? 0 : -1;
}

static void dense_forward(thimble_ctx *c, const thimble_layer *L, const float *in, float *out) {
    const int d = c->m->hp.n_embd, ff = c->m->hp.n_ff;
    op_matvec_w(c->f1, &L->ffn_up, 0, in, ff, d);
    if (L->ffn_gate.data) {
        op_matvec_w(c->f2, &L->ffn_gate, 0, in, ff, d);
        for (int i = 0; i < ff; i++) c->f1[i] *= op_silu(c->f2[i]);
    } else {
        for (int i = 0; i < ff; i++) c->f1[i] = op_gelu_erf(c->f1[i]);
    }
    op_matvec_w(out, &L->ffn_down, 0, c->f1, d, ff);
}

const thimble_ffn FFN_DENSE = { "dense", dense_load, dense_forward };
