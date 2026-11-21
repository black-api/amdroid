/* Numeric kernels (float32). Weights use the GGUF/ggml layout:
 * a linear layer with out x in weights is stored row-major, one row per output. */
#ifndef THIMBLE_OPS_H
#define THIMBLE_OPS_H

/* out[o] = sum_i W[o * n_in + i] * x[i] */
void op_matvec(float *out, const float *W, const float *x, int n_out, int n_in);

/* Same for a weight in any format; mat selects one matrix of a stack (MoE). */
#include "quant.h"
void op_matvec_w(float *out, const thimble_weight *W, int mat, const float *x, int n_out, int n_in);

void op_rmsnorm(float *out, const float *x, const float *w, int n, float eps);
void op_layernorm(float *out, const float *x, const float *w, const float *b, int n, float eps);

void  op_softmax(float *x, int n);
float op_dot(const float *a, const float *b, int n);
void  op_add(float *x, const float *y, int n);                 /* x += y */
void  op_axpy(float *x, float a, const float *y, int n);       /* x += a * y */

float op_silu(float x);
float op_gelu_erf(float x);
float op_sigmoid(float x);

/* Interleaved RoPE (pairs 2i, 2i+1 rotate with frequency base^(-2i/head_dim)),
 * matching ThimbleLM's Rotary module and ggml ROPE_TYPE_NORM. */
void op_rope(float *x, int n_heads, int head_dim, int pos, float base);

#endif
