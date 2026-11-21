/* Token sampling: greedy, temperature, top-k, top-p, min-p, repetition penalty. */
#include "sampler.h"

#include <math.h>
#include <stdlib.h>

thimble_sampling thimble_sampling_default(void) {
    thimble_sampling s = { 0 };
    s.top_p = 1.0f, s.repetition_penalty = 1.0f;
    return s;
}

uint64_t sampler_rng_next(uint64_t *state) {   /* xorshift64* */
    uint64_t x = *state ? *state : 0x9E3779B97F4A7C15ull;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static float rng_uniform(uint64_t *state) {
    return (float) (sampler_rng_next(state) >> 40) / (float) (1u << 24);
}

typedef struct {
    float logit;
    int   id;
} cand;

static int cmp_desc(const void *a, const void *b) {
    float x = ((const cand *) a)->logit, y = ((const cand *) b)->logit;
    return (x < y) - (x > y);
}

int sampler_sample(const float *logits, int n_vocab, const thimble_sampling *s, uint64_t *rng,
                   const int *history, int n_history) {
    cand *c = malloc(sizeof(cand) * (size_t) n_vocab);
    if (!c) return -1;
    for (int i = 0; i < n_vocab; i++) c[i] = (cand) { logits[i], i };

    if (s->repetition_penalty != 1.0f) {   /* HF style: always push repeated tokens down */
        for (int h = 0; h < n_history; h++) {
            int t = history[h];
            if (t < 0 || t >= n_vocab || c[t].logit != logits[t]) continue;   /* once per token */
            c[t].logit = logits[t] > 0 ? logits[t] / s->repetition_penalty : logits[t] * s->repetition_penalty;
        }
    }
    if (s->frequency_penalty != 0.0f || s->presence_penalty != 0.0f) {   /* OpenAI style */
        for (int h = 0; h < n_history; h++) {
            int t = history[h];
            if (t < 0 || t >= n_vocab) continue;
            int first = 1;
            for (int j = 0; j < h; j++) if (history[j] == t) { first = 0; break; }
            c[t].logit -= s->frequency_penalty + (first ? s->presence_penalty : 0.0f);
        }
    }
    for (int b = 0; b < s->n_logit_bias; b++) {
        int t = s->logit_bias_ids[b];
        if (t >= 0 && t < n_vocab) c[t].logit += s->logit_bias[b];
    }

    int best = 0;
    for (int i = 1; i < n_vocab; i++) if (c[i].logit > c[best].logit) best = i;
    if (s->temperature <= 0.0f) {
        free(c);
        return best;
    }

    qsort(c, (size_t) n_vocab, sizeof(cand), cmp_desc);
    int n = n_vocab;
    if (s->top_k > 0 && s->top_k < n) n = s->top_k;

    float mx = c[0].logit, sum = 0.0f;
    for (int i = 0; i < n; i++) {
        c[i].logit = expf((c[i].logit - mx) / s->temperature);
        sum += c[i].logit;
    }
    for (int i = 0; i < n; i++) c[i].logit /= sum;

    if (s->min_p > 0.0f) {
        float cut = s->min_p * c[0].logit;
        int   k   = 1;
        while (k < n && c[k].logit >= cut) k++;
        n = k;
    }
    if (s->top_p < 1.0f) {
        float acc = 0.0f;
        int   k   = 0;
        while (k < n) {
            acc += c[k].logit;
            k++;
            if (acc >= s->top_p) break;
        }
        n = k;
    }
    float total = 0.0f;
    for (int i = 0; i < n; i++) total += c[i].logit;
    float r  = rng_uniform(rng) * total, acc = 0.0f;
    int   id = c[n - 1].id;
    for (int i = 0; i < n; i++) {
        acc += c[i].logit;
        if (r < acc) { id = c[i].id; break; }
    }
    free(c);
    return id;
}
