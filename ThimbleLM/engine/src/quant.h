/* Weight formats: F32, F16, BF16 and the ggml block quantizations
 * Q8_0, Q5_1, Q5_0, Q4_1, Q4_0 (identical bit layout to ggml/llama.cpp).
 *
 * To add a format: give it a ggml type id below, a block size / byte size in
 * quant.c:type_traits, and a dequantize_row case. Matmuls pick it up through
 * op_matvec_w. */
#ifndef THIMBLE_QUANT_H
#define THIMBLE_QUANT_H

#include <stddef.h>
#include <stdint.h>

enum {   /* ggml type ids as stored in GGUF */
    QT_F32 = 0, QT_F16 = 1, QT_Q4_0 = 2, QT_Q4_1 = 3, QT_Q5_0 = 6, QT_Q5_1 = 7, QT_Q8_0 = 8,
    QT_BF16 = 30,
};

/* A weight matrix (or a stack of them, e.g. MoE experts) in any format.
 * Rows run along ne0 (the input dimension). */
typedef struct {
    const void *data;     /* NULL when the tensor is absent */
    int         type;
    int64_t     ne[3];
    size_t      row_bytes;
} thimble_weight;

int         quant_supported(int type);
const char *quant_name(int type);
int         quant_block_size(int type);    /* elements per block */
size_t      quant_row_bytes(int type, int64_t n);   /* 0 if n is not a multiple of the block size */

/* y[0..n) = dequantized row (n must be a multiple of the block size). */
void dequantize_row(int type, const void *row, float *y, int n);

/* dot(row, x) over n elements, dequantizing on the fly. */
float quant_dot(int type, const void *row, const float *x, int n);

/* Build lookup tables; called once by thimble_model_load. */
void quant_init(void);

float fp16_to_f32(uint16_t h);
float bf16_to_f32(uint16_t h);

#endif
