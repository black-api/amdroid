/* Mixture-of-Experts FFN: softmax router, top-k experts, renormalized gates,
 * SwiGLU experts (as exported by thimble export-gguf). */
#include "model.h"
#include "ops.h"

#include <string.h>

static int moe_load(thimble_model *m, thimble_layer *L, int il) {
    const int d = m->hp.n_embd, ff = m->hp.n_ff_exp, E = m->hp.n_expert;
    L->router   = model_weight(m, il, "ffn_gate_inp.weight", 1, d, E, -1);
    L->exp_gate = model_weight(m, il, "ffn_gate_exps.weight", 1, d, ff, E);
    L->exp_up   = model_weight(m, il, "ffn_up_exps.weight", 1, d, ff, E);
    L->exp_down = model_weight(m, il, "ffn_down_exps.weight", 1, ff, d, E);
    return (L->router.data && L->exp_gate.data && L->exp_up.data && L->exp_down.data) ? 0 : -1;
}

static void moe_forward(thimble_ctx *c, const thimble_layer *L, const float *in, float *out) {
    const thimble_hparams *hp = &c->m->hp;
    const int d = hp->n_embd, ff = hp->n_ff_exp, E = hp->n_expert, K = hp->n_expert_used;

    op_matvec_w(c->probs, &L->router, 0, in, E, d);
    op_softmax(c->probs, E);

    float wsum = 0.0f;
    for (int k = 0; k < K; k++) {   /* top-k by repeated argmax (K is small) */
        int best = -1;
        for (int e = 0; e < E; e++) {
            int taken = 0;
            for (int j = 0; j < k; j++) taken |= c->sel[j] == e;
            if (!taken && (best < 0 || c->probs[e] > c->probs[best])) best = e;
        }
        c->sel[k] = best;
        wsum += c->probs[best];
    }

    memset(out, 0, sizeof(float) * (size_t) d);
    for (int k = 0; k < K; k++) {
        const int e = c->sel[k];
        op_matvec_w(c->f1, &L->exp_up, e, in, ff, d);
        op_matvec_w(c->f2, &L->exp_gate, e, in, ff, d);
        for (int i = 0; i < ff; i++) c->f1[i] *= op_silu(c->f2[i]);
        op_matvec_w(c->f3, &L->exp_down, e, c->f1, d, ff);
        op_axpy(out, c->probs[e] / wsum, c->f3, d);
    }
}

const thimble_ffn FFN_MOE = { "moe", moe_load, moe_forward };
