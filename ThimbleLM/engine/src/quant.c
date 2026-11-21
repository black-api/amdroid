#include "quant.h"

#include <string.h>

/* ---- block layouts (ggml) --------------------------------------------- */

#define QK 32

typedef struct { uint16_t d; int8_t qs[QK]; } block_q8_0;                      /* 34 bytes */
typedef struct { uint16_t d; uint8_t qs[QK / 2]; } block_q4_0;                 /* 18 bytes */
typedef struct { uint16_t d, m; uint8_t qs[QK / 2]; } block_q4_1;              /* 20 bytes */
typedef struct { uint16_t d; uint8_t qh[4]; uint8_t qs[QK / 2]; } block_q5_0;  /* 22 bytes */
typedef struct { uint16_t d, m; uint8_t qh[4]; uint8_t qs[QK / 2]; } block_q5_1; /* 24 bytes */

_Static_assert(sizeof(block_q8_0) == 34, "q8_0 layout");
_Static_assert(sizeof(block_q4_0) == 18, "q4_0 layout");
_Static_assert(sizeof(block_q4_1) == 20, "q4_1 layout");
_Static_assert(sizeof(block_q5_0) == 22, "q5_0 layout");
_Static_assert(sizeof(block_q5_1) == 24, "q5_1 layout");

typedef struct {
    int         type;
    const char *name;
    int         block;
    size_t      bytes;   /* per block */
} type_traits;

static const type_traits TRAITS[] = {
    { QT_F32,  "F32",  1,  4 },
    { QT_F16,  "F16",  1,  2 },
    { QT_BF16, "BF16", 1,  2 },
    { QT_Q8_0, "Q8_0", QK, sizeof(block_q8_0) },
    { QT_Q5_1, "Q5_1", QK, sizeof(block_q5_1) },
    { QT_Q5_0, "Q5_0", QK, sizeof(block_q5_0) },
    { QT_Q4_1, "Q4_1", QK, sizeof(block_q4_1) },
    { QT_Q4_0, "Q4_0", QK, sizeof(block_q4_0) },
};

static const type_traits *traits(int type) {
    for (size_t i = 0; i < sizeof(TRAITS) / sizeof(TRAITS[0]); i++)
        if (TRAITS[i].type == type) return &TRAITS[i];
    return NULL;
}

int         quant_supported(int type) { return traits(type) != NULL; }
const char *quant_name(int type) { const type_traits *t = traits(type); return t ? t->name : "?"; }
int         quant_block_size(int type) { const type_traits *t = traits(type); return t ? t->block : 0; }

size_t quant_row_bytes(int type, int64_t n) {
    const type_traits *t = traits(type);
    if (!t || n % t->block) return 0;
    return (size_t) (n / t->block) * t->bytes;
}

/* ---- scalar conversions ------------------------------------------------ */

float fp16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {   /* subnormal: normalize */
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            bits = sign | (exp << 23) | ((mant & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

float bf16_to_f32(uint16_t h) {
    uint32_t bits = (uint32_t) h << 16;
    float    f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ---- dequantization ----------------------------------------------------- */

static void deq_q8_0(const block_q8_0 *b, float *y) {
    const float d = fp16_to_f32(b->d);
    for (int j = 0; j < QK; j++) y[j] = b->qs[j] * d;
}

static void deq_q4_0(const block_q4_0 *b, float *y) {
    const float d = fp16_to_f32(b->d);
    for (int j = 0; j < QK / 2; j++) {
        y[j]          = ((b->qs[j] & 0x0F) - 8) * d;
        y[j + QK / 2] = ((b->qs[j] >> 4) - 8) * d;
    }
}

static void deq_q4_1(const block_q4_1 *b, float *y) {
    const float d = fp16_to_f32(b->d), m = fp16_to_f32(b->m);
    for (int j = 0; j < QK / 2; j++) {
        y[j]          = (b->qs[j] & 0x0F) * d + m;
        y[j + QK / 2] = (b->qs[j] >> 4) * d + m;
    }
}

static void deq_q5_0(const block_q5_0 *b, float *y) {
    const float d = fp16_to_f32(b->d);
    uint32_t    qh;
    memcpy(&qh, b->qh, 4);
    for (int j = 0; j < QK / 2; j++) {
        const int h0 = ((qh >> j) << 4) & 0x10;
        const int h1 = (qh >> (j + 12)) & 0x10;
        y[j]          = (((b->qs[j] & 0x0F) | h0) - 16) * d;
        y[j + QK / 2] = (((b->qs[j] >> 4) | h1) - 16) * d;
    }
}

static void deq_q5_1(const block_q5_1 *b, float *y) {
    const float d = fp16_to_f32(b->d), m = fp16_to_f32(b->m);
    uint32_t    qh;
    memcpy(&qh, b->qh, 4);
    for (int j = 0; j < QK / 2; j++) {
        const int h0 = ((qh >> j) << 4) & 0x10;
        const int h1 = (qh >> (j + 12)) & 0x10;
        y[j]          = ((b->qs[j] & 0x0F) | h0) * d + m;
        y[j + QK / 2] = ((b->qs[j] >> 4) | h1) * d + m;
    }
}

void dequantize_row(int type, const void *row, float *y, int n) {
    const uint8_t *p = row;
    switch (type) {
        case QT_F32: memcpy(y, row, sizeof(float) * (size_t) n); break;
        case QT_F16:
            for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); y[i] = fp16_to_f32(h); }
            break;
        case QT_BF16:
            for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, p + 2 * i, 2); y[i] = bf16_to_f32(h); }
            break;
        case QT_Q8_0: for (int b = 0; b < n / QK; b++) deq_q8_0((const block_q8_0 *) p + b, y + b * QK); break;
        case QT_Q4_0: for (int b = 0; b < n / QK; b++) deq_q4_0((const block_q4_0 *) p + b, y + b * QK); break;
        case QT_Q4_1: for (int b = 0; b < n / QK; b++) deq_q4_1((const block_q4_1 *) p + b, y + b * QK); break;
        case QT_Q5_0: for (int b = 0; b < n / QK; b++) deq_q5_0((const block_q5_0 *) p + b, y + b * QK); break;
        case QT_Q5_1: for (int b = 0; b < n / QK; b++) deq_q5_1((const block_q5_1 *) p + b, y + b * QK); break;
        default: memset(y, 0, sizeof(float) * (size_t) n);
    }
}

/* ---- fused dot kernels (no temporary buffer; loops vectorize) ---------- */

static float F16_LUT[65536];
static int   f16_lut_ready;

static void f16_lut_init(void) {
    if (f16_lut_ready) return;
    for (uint32_t h = 0; h < 65536; h++) F16_LUT[h] = fp16_to_f32((uint16_t) h);
    f16_lut_ready = 1;
}

void quant_init(void) { f16_lut_init(); }

static float dot_f16(const uint16_t *w, const float *x, int n) {
    float acc = 0.0f;
    for (int i = 0; i < n; i++) acc += F16_LUT[w[i]] * x[i];
    return acc;
}

static float dot_bf16(const uint16_t *w, const float *x, int n) {
    float acc = 0.0f;
    for (int i = 0; i < n; i++) {
        uint32_t bits = (uint32_t) w[i] << 16;
        float    f;
        memcpy(&f, &bits, 4);
        acc += f * x[i];
    }
    return acc;
}

static float dot_q8_0(const block_q8_0 *b, const float *x, int nb) {
    float acc = 0.0f;
    for (int k = 0; k < nb; k++, x += QK) {
        float s = 0.0f;
        for (int j = 0; j < QK; j++) s += (float) b[k].qs[j] * x[j];
        acc += s * F16_LUT[b[k].d];
    }
    return acc;
}

static float dot_q4_0(const block_q4_0 *b, const float *x, int nb) {
    float acc = 0.0f;
    for (int k = 0; k < nb; k++, x += QK) {
        float s = 0.0f;
        for (int j = 0; j < QK / 2; j++)
            s += (float) ((b[k].qs[j] & 0x0F) - 8) * x[j] + (float) ((b[k].qs[j] >> 4) - 8) * x[j + QK / 2];
        acc += s * F16_LUT[b[k].d];
    }
    return acc;
}

static float dot_q4_1(const block_q4_1 *b, const float *x, int nb) {
    float acc = 0.0f;
    for (int k = 0; k < nb; k++, x += QK) {
        float s = 0.0f, sx = 0.0f;
        for (int j = 0; j < QK / 2; j++) {
            s  += (float) (b[k].qs[j] & 0x0F) * x[j] + (float) (b[k].qs[j] >> 4) * x[j + QK / 2];
            sx += x[j] + x[j + QK / 2];
        }
        acc += s * F16_LUT[b[k].d] + sx * F16_LUT[b[k].m];
    }
    return acc;
}

static float dot_q5_0(const block_q5_0 *b, const float *x, int nb) {
    float acc = 0.0f;
    for (int k = 0; k < nb; k++, x += QK) {
        uint32_t qh;
        memcpy(&qh, b[k].qh, 4);
        float s = 0.0f;
        for (int j = 0; j < QK / 2; j++) {
            const int h0 = ((qh >> j) << 4) & 0x10, h1 = (qh >> (j + 12)) & 0x10;
            s += (float) (((b[k].qs[j] & 0x0F) | h0) - 16) * x[j] +
                 (float) (((b[k].qs[j] >> 4) | h1) - 16) * x[j + QK / 2];
        }
        acc += s * F16_LUT[b[k].d];
    }
    return acc;
}

static float dot_q5_1(const block_q5_1 *b, const float *x, int nb) {
    float acc = 0.0f;
    for (int k = 0; k < nb; k++, x += QK) {
        uint32_t qh;
        memcpy(&qh, b[k].qh, 4);
        float s = 0.0f, sx = 0.0f;
        for (int j = 0; j < QK / 2; j++) {
            const int h0 = ((qh >> j) << 4) & 0x10, h1 = (qh >> (j + 12)) & 0x10;
            s  += (float) ((b[k].qs[j] & 0x0F) | h0) * x[j] + (float) ((b[k].qs[j] >> 4) | h1) * x[j + QK / 2];
            sx += x[j] + x[j + QK / 2];
        }
        acc += s * F16_LUT[b[k].d] + sx * F16_LUT[b[k].m];
    }
    return acc;
}

float quant_dot(int type, const void *row, const float *x, int n) {
    switch (type) {
        case QT_F32: {
            const float *w   = row;
            float        acc = 0.0f;
            for (int i = 0; i < n; i++) acc += w[i] * x[i];
            return acc;
        }
        case QT_F16:  return dot_f16(row, x, n);
        case QT_BF16: return dot_bf16(row, x, n);
        case QT_Q8_0: return dot_q8_0(row, x, n / QK);
        case QT_Q4_0: return dot_q4_0(row, x, n / QK);
        case QT_Q4_1: return dot_q4_1(row, x, n / QK);
        case QT_Q5_0: return dot_q5_0(row, x, n / QK);
        case QT_Q5_1: return dot_q5_1(row, x, n / QK);
        default:      return 0.0f;
    }
}
