/* thimble-v4 gated short convolution (LFM2-style):
 *   B, C, u = in_proj(x);  y = out_proj(C * causal_depthwise_conv_k(B * u))
 * State: B * u for every position (like a KV cache), so the context can be
 * rewound to any earlier position and forked; the conv reads the k - 1 most
 * recent entries before the current position. */
#include "model.h"
#include "ops.h"

#include <string.h>

static int conv_load(thimble_model *m, thimble_layer *L, int il) {
    const int d = m->hp.n_embd, k = m->hp.conv_kernel;
    if (k < 1) return model_error(m, "invalid shortconv.l_cache %d", k);
    L->conv_in  = model_weight(m, il, "shortconv.in_proj.weight", 1, d, 3 * d, -1);
    L->conv_w   = model_tensor(m, il, "shortconv.conv.weight", 1, k, d, -1);
    L->conv_out = model_weight(m, il, "shortconv.out_proj.weight", 1, d, d, -1);
    return (L->conv_in.data && L->conv_w && L->conv_out.data) ? 0 : -1;
}

static size_t conv_state(const thimble_model *m, const thimble_layer *L, int n_ctx) {
    (void) L;
    return (size_t) n_ctx * (size_t) m->hp.n_embd;   /* B * u per position */
}

static void conv_forward(thimble_ctx *c, const thimble_layer *L, float *state, const float *in,
                         float *out) {
    const int d = c->m->hp.n_embd, k = c->m->hp.conv_kernel, pos = c->pos;
    op_matvec_w(c->bcx, &L->conv_in, 0, in, 3 * d, d);
    const float *B = c->bcx, *C = c->bcx + d, *U = c->bcx + 2 * d;
    float       *y = c->f3;
    float       *bx = state + (size_t) pos * d;   /* this position's B * u */
    for (int ch = 0; ch < d; ch++) bx[ch] = B[ch] * U[ch];
    for (int ch = 0; ch < d; ch++) {
        const float *w   = L->conv_w + (size_t) ch * k;   /* taps oldest -> newest */
        float        acc = w[k - 1] * bx[ch];
        for (int j = 0; j < k - 1; j++) {
            const int p = pos - (k - 1) + j;   /* positions before 0 are zero padding */
            if (p >= 0) acc += w[j] * state[(size_t) p * d + ch];
        }
        y[ch] = C[ch] * acc;
    }
    op_matvec_w(out, &L->conv_out, 0, y, d, d);
}

const thimble_mixer MIXER_SHORTCONV = { "shortconv", conv_load, conv_state, conv_forward };
