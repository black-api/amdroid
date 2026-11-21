#ifndef THIMBLE_SAMPLER_H
#define THIMBLE_SAMPLER_H

#include "../include/thimble.h"

uint64_t sampler_rng_next(uint64_t *state);

/* Pick the next token id from logits (history feeds the repetition penalty). */
int sampler_sample(const float *logits, int n_vocab, const thimble_sampling *s, uint64_t *rng,
                   const int *history, int n_history);

#endif
