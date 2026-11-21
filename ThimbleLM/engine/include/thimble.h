/*
 * Thimble engine - public C API.
 *
 * A small, dependency-free inference engine for ThimbleLM models
 * (thimble-v1, thimble-v2, thimble-v4) exported with
 * `python3 -m thimble export-gguf CHECKPOINT OUT.gguf` (GGUF archs
 * `thimble` and `thimble-hybrid`).
 */
#ifndef THIMBLE_H
#define THIMBLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thimble_model thimble_model;
typedef struct thimble_ctx   thimble_ctx;

/* ---- model ------------------------------------------------------------ */

/* Load a GGUF file. On failure returns NULL and writes a message to err. */
thimble_model *thimble_model_load(const char *path, char *err, size_t err_len);
void           thimble_model_free(thimble_model *model);

int         thimble_n_vocab(const thimble_model *model);
int         thimble_n_ctx_train(const thimble_model *model);
int         thimble_eos_id(const thimble_model *model);
int         thimble_has_thinking(const thimble_model *model);   /* trained with think mode */
const char *thimble_arch(const thimble_model *model);           /* "thimble" | "thimble-hybrid" */
/* Human-readable summary (layer layout, sizes). Writes at most len bytes. */
void        thimble_describe(const thimble_model *model, char *buf, size_t len);

/* ---- tokenizer -------------------------------------------------------- */

/* Tokenize UTF-8 text (special tokens like <|user|> are recognized).
 * Writes up to max_out ids into out and returns the total count. */
int thimble_tokenize(const thimble_model *model, const char *text, int *out, int max_out);

/* Bytes of one token (0 for special/unused tokens unless with_special).
 * Writes up to len bytes (not NUL-terminated); returns the full length. */
int thimble_token_piece(const thimble_model *model, int id, char *buf, int len, int with_special);

/* ---- context / forward ------------------------------------------------ */

/* n_ctx <= 0 uses the model's training context length. */
thimble_ctx *thimble_ctx_new(const thimble_model *model, int n_ctx);
void         thimble_ctx_free(thimble_ctx *ctx);
void         thimble_ctx_reset(thimble_ctx *ctx);   /* clear KV cache and conv state */
int          thimble_ctx_pos(const thimble_ctx *ctx);
int          thimble_ctx_n_ctx(const thimble_ctx *ctx);

/* Rewind to the first n_keep tokens (0 <= n_keep <= pos) so a prompt sharing
 * that prefix continues from the cache. Works for every layer type: attention
 * keeps K/V and short-conv keeps its input history per position.
 * Returns 0, or -1 if n_keep is out of range. */
int          thimble_ctx_truncate(thimble_ctx *ctx, int n_keep);

/* Fork: make dst hold the same tokens/state as src (same model, dst->n_ctx
 * >= src pos). Cheap: copies only the positions in use. Returns 0 or -1. */
int          thimble_ctx_copy(thimble_ctx *dst, const thimble_ctx *src);

/* Append one token at the next position; returns n_vocab logits
 * (valid until the next call) or NULL when the context is full. */
const float *thimble_eval(thimble_ctx *ctx, int token);

/* ---- chat ------------------------------------------------------------- */

typedef struct {
    const char *role;      /* "system" | "user" | "assistant" */
    const char *content;
} thimble_message;

enum {
    THIMBLE_THINK_DEFAULT = -1,   /* on for think-mode models, plain template otherwise */
    THIMBLE_THINK_OFF     =  0,   /* prefill an empty <|think|><|/think|> block */
    THIMBLE_THINK_ON      =  1,   /* open <|think|> for the model to reason in */
};

/* Render a conversation plus the assistant marker (ThimbleLM chat template).
 * Returns a malloc'd string (free with free()) or NULL if thinking is
 * requested from a model trained without it. */
char *thimble_render_chat(const thimble_model *model, const thimble_message *msgs, int n_msgs,
                          int thinking);

/* ---- sampling / generation -------------------------------------------- */

typedef struct {
    float        temperature;         /* <= 0: greedy */
    int          top_k;               /* 0: off */
    float        top_p;               /* 1: off */
    float        min_p;               /* 0: off */
    float        repetition_penalty;  /* 1: off (HF style, over the history) */
    uint64_t     seed;
    float        frequency_penalty;   /* 0: off (OpenAI style: logit -= count * f) */
    float        presence_penalty;    /* 0: off (logit -= p if the token occurred) */
    const int   *logit_bias_ids;      /* optional additive biases */
    const float *logit_bias;
    int          n_logit_bias;
} thimble_sampling;

thimble_sampling thimble_sampling_default(void);   /* greedy */

/* Pick the next token from logits. history (generated tokens) feeds the
 * penalties; rng is the caller's state (seed it with sampling->seed). */
int thimble_sample(const thimble_model *model, const float *logits, const thimble_sampling *sampling,
                   uint64_t *rng, const int *history, int n_history);

/* Special token ids (-1 when the vocabulary lacks them). */
typedef struct { int bos, eos, pad, unk, think, think_end; } thimble_special;
thimble_special thimble_special_tokens(const thimble_model *model);

/* Called once per generated token. is_reasoning is 1 inside a think block.
 * Return non-zero to stop generation. */
typedef int (*thimble_token_cb)(int token, const char *piece, int piece_len, int is_reasoning,
                                void *user);

typedef struct {
    int n_generated;
    int stopped_by_eos;
    int n_reasoning_tokens;
} thimble_gen_result;

/* Feed the prompt, then sample up to max_new tokens (stops at <|eos|>).
 * If the prompt ends with <|think|> generation starts in reasoning mode and
 * switches to the answer after <|/think|>. Returns -1 on error. */
int thimble_generate(thimble_ctx *ctx, const int *prompt, int n_prompt,
                     const thimble_sampling *sampling, int max_new,
                     thimble_token_cb cb, void *user, thimble_gen_result *result);

/* ---- voice (ThimbleTTS) ------------------------------------------------- */

/* A voice is one .voice file made by `python -m thimble voice pack`. It holds
 * the networks, the pronunciation dictionaries and the fallback model, so
 * speaking needs nothing else on disk and no Python. */
typedef struct thimble_voice thimble_voice;

/* Where voices live: $THIMBLE_VOICE (a file), else $THIMBLE_VOICE_DIR, else
 * $XDG_CACHE_HOME/thimble/voices, else ~/.cache/thimble/voices. Writes the
 * directory to buf and returns its length (0 if no home directory is known). */
size_t thimble_voice_dir(char *buf, size_t len);

/* Find a voice file: `name` (a path, or a voice name inside the voice directory,
 * with or without ".voice"), or when name is NULL/"" the $THIMBLE_VOICE file or
 * the first *.voice in the directory. Returns 1 and fills path, or 0. */
int thimble_voice_find(const char *name, char *path, size_t len);

thimble_voice *thimble_voice_load(const char *path, char *err, size_t err_len);
void           thimble_voice_free(thimble_voice *voice);
int            thimble_voice_sample_rate(const thimble_voice *voice);
/* Threads used for synthesis. n > 0 sets it (capped at 8); any n returns the
 * value in force. The default is $THIMBLE_VOICE_THREADS, else the core count.
 * The waveform is the same whatever the thread count. */
int            thimble_voice_threads(int n);
const char    *thimble_voice_name(const thimble_voice *voice);
const char    *thimble_voice_license(const thimble_voice *voice);   /* MIT/Apache notices that ship with it */

/* Speak text. Returns the number of 16-bit mono samples (24 kHz; free *pcm with
 * free()), or <0: -2 no speakable text, -3 too long, -1 other (message in err). */
int thimble_voice_speak(const thimble_voice *voice, const char *text, int16_t **pcm,
                        char *err, size_t err_len);

/* Lower-level pieces, used by tests and tools. */
int thimble_voice_ids(const thimble_voice *voice, const char *text, int *ids, int max_ids,
                      char *err, size_t err_len);                   /* text -> phoneme ids */
int thimble_voice_phonemes(const thimble_voice *voice, const char *text, char *out, size_t cap);
int thimble_voice_synth_ids(const thimble_voice *voice, const int *ids, int n, float **wav);

#ifdef __cplusplus
}
#endif

#endif /* THIMBLE_H */
