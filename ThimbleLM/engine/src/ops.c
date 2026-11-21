#include "ops.h"

#include <math.h>
#include <stdint.h>

void op_matvec(float *out, const float *W, const float *x, int n_out, int n_in) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (n_out * n_in > 65536)
#endif
    for (int o = 0; o < n_out; o++) {
        const float *row = W + (long) o * n_in;
        float        acc = 0.0f;
        for (int i = 0; i < n_in; i++) acc += row[i] * x[i];
        out[o] = acc;
    }
}

void op_matvec_w(float *out, const thimble_weight *W, int mat, const float *x, int n_out, int n_in) {
    const uint8_t *base = (const uint8_t *) W->data + (size_t) mat * (size_t) n_out * W->row_bytes;
    if (W->type == QT_F32) {
        op_matvec(out, (const float *) base, x, n_out, n_in);
        return;
    }
#ifdef _OPENMP
#pragma omp parallel for schedule(static) if (n_out * n_in > 65536)
#endif
    for (int o = 0; o < n_out; o++) out[o] = quant_dot(W->type, base + (size_t) o * W->row_bytes, x, n_in);
}

void op_rmsnorm(float *out, const float *x, const float *w, int n, float eps) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float scale = 1.0f / sqrtf(ss / (float) n + eps);
    for (int i = 0; i < n; i++) out[i] = x[i] * scale * w[i];
}

void op_layernorm(float *out, const float *x, const float *w, const float *b, int n, float eps) {
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= (float) n;
    float var = 0.0f;
    for (int i = 0; i < n; i++) var += (x[i] - mean) * (x[i] - mean);
    var /= (float) n;
    float scale = 1.0f / sqrtf(var + eps);
    for (int i = 0; i < n; i++) out[i] = (x[i] - mean) * scale * w[i] + (b ? b[i] : 0.0f);
}

void op_softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        x[i] = expf(x[i] - mx);
        sum += x[i];
    }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

float op_dot(const float *a, const float *b, int n) {
    float acc = 0.0f;
    for (int i = 0; i < n; i++) acc += a[i] * b[i];
    return acc;
}

void op_add(float *x, const float *y, int n) {
    for (int i = 0; i < n; i++) x[i] += y[i];
}

void op_axpy(float *x, float a, const float *y, int n) {
    for (int i = 0; i < n; i++) x[i] += a * y[i];
}

float op_silu(float x) { return x / (1.0f + expf(-x)); }

float op_gelu_erf(float x) { return 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); }

float op_sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

void op_rope(float *x, int n_heads, int head_dim, int pos, float base) {
    for (int i = 0; i < head_dim / 2; i++) {
        /* same float32 math as torch: inv_freq = 1 / base^(2i/d); angle = pos * inv_freq */
        float inv_freq = 1.0f / powf(base, (float) (2 * i) / (float) head_dim);
        float angle    = (float) pos * inv_freq;
        float c = cosf(angle), s = sinf(angle);
        for (int h = 0; h < n_heads; h++) {
            float *p  = x + h * head_dim + 2 * i;
            float  x0 = p[0], x1 = p[1];
            p[0] = x0 * c - x1 * s;
            p[1] = x1 * c + x0 * s;
        }
    }
}
