/* Internal model / context structures and the layer registry.
 *
 * A block is:  x += mixer(norm1(x));  x += ffn(norm2(x))
 *
 * Token mixers (attention, short conv) and FFNs (dense, MoE) are pluggable:
 * each lives in its own file and exposes a descriptor below. To add a new
 * layer type:
 *   1. write src/mixer_xxx.c (or src/ffn_xxx.c) implementing the descriptor,
 *   2. declare it here (extern const ...),
 *   3. pick it in model.c:choose_mixer() / choose_ffn() from the GGUF tensors.
 */
#ifndef THIMBLE_MODEL_H
#define THIMBLE_MODEL_H

#include "../include/thimble.h"
#include "gguf.h"
#include "quant.h"
#include "tokenizer.h"

typedef struct thimble_layer thimble_layer;

typedef struct thimble_mixer {
    const char *name;
    /* bind weights; return 0 or -1 (message via model_error) */
    int    (*load)(thimble_model *m, thimble_layer *L, int il);
    /* floats of per-sequence state this layer needs (KV cache, conv state) */
    size_t (*state_floats)(const thimble_model *m, const thimble_layer *L, int n_ctx);
    /* out = mixer(in) for the token at ctx->pos; may update state */
    void   (*forward)(thimble_ctx *c, const thimble_layer *L, float *state, const float *in, float *out);
} thimble_mixer;

typedef struct thimble_ffn {
    const char *name;
    int  (*load)(thimble_model *m, thimble_layer *L, int il);
    void (*forward)(thimble_ctx *c, const thimble_layer *L, const float *in, float *out);
} thimble_ffn;

extern const thimble_mixer MIXER_ATTENTION;   /* mixer_attn.c */
extern const thimble_mixer MIXER_SHORTCONV;   /* mixer_conv.c */
extern const thimble_ffn   FFN_DENSE;         /* ffn_dense.c  */
extern const thimble_ffn   FFN_MOE;           /* ffn_moe.c    */

struct thimble_layer {
    const thimble_mixer *mixer;
    const thimble_ffn   *ffn;
    int                  n_kv_head;           /* attention layers */

    const float *attn_norm, *attn_norm_b, *ffn_norm, *ffn_norm_b;

    /* matmul weights are thimble_weight (any format, see quant.h);
     * 1-D vectors (norms, conv taps) are always F32 */
    /* attention */
    thimble_weight wq, wk, wv, wo, wgate;
    const float   *q_norm, *k_norm;
    /* short conv */
    thimble_weight conv_in, conv_out;
    const float   *conv_w;
    /* dense FFN (ffn_gate absent: GELU, else SwiGLU) */
    thimble_weight ffn_up, ffn_gate, ffn_down;
    /* MoE (expert stacks: ne2 = n_expert) */
    thimble_weight router, exp_gate, exp_up, exp_down;
};

typedef struct {
    int   n_vocab, n_ctx_train, n_embd, n_layer, n_head, head_dim;
    int   n_ff, n_ff_exp, n_expert, n_expert_used, conv_kernel;
    float rope_base, rms_eps, ln_eps;
    int   layer_norm;    /* LayerNorm (with bias) instead of RMSNorm */
    int   learned_pos;   /* position_embd instead of RoPE */
    int   thinking;      /* trained with the think template */
    int   copy_head;     /* pointer/copy head mixed into the output layer */
    int   copy_dim;      /* pointer query/key width */
} thimble_hparams;

struct thimble_model {
    gguf_file         gguf;
    char              arch[32];
    char              name[128];
    thimble_hparams   hp;
    thimble_weight    tok_embd, output;
    const float      *pos_embd, *out_norm, *out_norm_b;
    /* pointer/copy head: q/k projections [copy_dim x n_embd], gate [1 x n_embd] + bias */
    const float      *copy_q, *copy_k, *copy_gate_w, *copy_gate_b;
    thimble_layer    *layers;
    thimble_tokenizer tok;
    char             *err;        /* load-time error buffer */
    size_t            err_len;
};

struct thimble_ctx {
    const thimble_model *m;
    int                  n_ctx, pos;
    float              **state;    /* per layer, from mixer->state_floats */
    float *x, *h, *y;              /* residual stream and temporaries [n_embd] */
    float *q, *k, *v, *att, *gate; /* attention scratch */
    float *scores;                 /* [n_ctx] */
    float *bcx;                    /* short conv in_proj output [3 * n_embd] */
    float *f1, *f2, *f3;           /* FFN scratch [max(n_ff, n_ff_exp, n_embd)] */
    float *probs;                  /* MoE router [n_expert] */
    int   *sel;                    /* MoE selected experts [n_expert_used] */
    float *logits;                 /* [n_vocab] */
    /* pointer/copy head: the tokens seen so far are the copy source, so unlike
     * every other piece of state the context ids have to be kept. */
    int   *ctx_tokens;             /* [n_ctx] */
    float *cq;                     /* pointer query [copy_dim] */
    float *ckeys;                  /* pointer keys, one per position [n_ctx x copy_dim] */
    float *cemb;                   /* dequantized token embedding [n_embd] */
    float *cscores;                /* pointer scores [n_ctx] */
};

/* Helpers for layer loaders (model.c). */
int          model_error(thimble_model *m, const char *fmt, ...);
/* Fetch an F32 tensor "blk.<il>.<name>" (il < 0: no prefix) and check its
 * shape (ggml order, -1 = don't care). Returns NULL if missing; when required,
 * also records an error. */
const float *model_tensor(thimble_model *m, int il, const char *name, int required,
                          int64_t ne0, int64_t ne1, int64_t ne2);

/* Like model_tensor but for matmul weights, which may be F16/BF16/quantized.
 * .data is NULL when absent. */
thimble_weight model_weight(thimble_model *m, int il, const char *name, int required,
                            int64_t ne0, int64_t ne1, int64_t ne2);

/* Apply the model's block norm (RMSNorm or LayerNorm). */
void model_norm(const thimble_model *m, float *out, const float *x, const float *w, const float *b);

#endif
