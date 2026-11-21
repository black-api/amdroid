/* Generation loop: prefill the prompt, then sample until <|eos|> or max_new.
 * Tokens between <|think|> and <|/think|> are reported as reasoning. */
#include "model.h"
#include "sampler.h"

int thimble_sample(const thimble_model *m, const float *logits, const thimble_sampling *s, uint64_t *rng,
                   const int *history, int n_history) {
    thimble_sampling def = thimble_sampling_default();
    return sampler_sample(logits, m->hp.n_vocab, s ? s : &def, rng, history, n_history);
}

thimble_special thimble_special_tokens(const thimble_model *m) {
    thimble_special sp = { m->tok.bos, m->tok.eos, m->tok.pad, m->tok.unk, m->tok.think, m->tok.think_end };
    return sp;
}

#include <stdlib.h>

int thimble_generate(thimble_ctx *c, const int *prompt, int n_prompt, const thimble_sampling *s,
                     int max_new, thimble_token_cb cb, void *user, thimble_gen_result *res) {
    const thimble_model *m = c->m;
    thimble_gen_result   r = { 0, 0, 0 };
    thimble_sampling     def = thimble_sampling_default();
    if (!s) s = &def;
    if (n_prompt <= 0) return -1;

    const float *logits = NULL;
    for (int i = 0; i < n_prompt; i++) {
        logits = thimble_eval(c, prompt[i]);
        if (!logits) return -1;
    }
    int reasoning = m->tok.think >= 0 && prompt[n_prompt - 1] == m->tok.think;

    int *history = malloc(sizeof(int) * (size_t) (n_prompt + max_new + 1));
    if (!history) return -1;
    int n_hist = 0;
    uint64_t rng = s->seed ? s->seed : 0x5EED;
    char piece[256];

    for (int step = 0; step < max_new; step++) {
        int tok = sampler_sample(logits, m->hp.n_vocab, s, &rng, history, n_hist);
        if (tok < 0) break;
        history[n_hist++] = tok;
        r.n_generated++;
        if (tok == m->tok.eos) {
            r.stopped_by_eos = 1;
            break;
        }
        int was_reasoning = reasoning;
        if (reasoning && tok == m->tok.think_end) reasoning = 0;
        if (was_reasoning) r.n_reasoning_tokens++;
        int len = tokenizer_piece(&m->tok, tok, piece, (int) sizeof(piece), 0);
        if (len > (int) sizeof(piece)) len = (int) sizeof(piece);
        if (cb && cb(tok, piece, len, was_reasoning && tok != m->tok.think_end, user)) break;
        if (step + 1 < max_new) {
            logits = thimble_eval(c, tok);
            if (!logits) break;   /* context full */
        }
    }
    free(history);
    if (res) *res = r;
    return r.n_generated;
}
