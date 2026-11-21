/*
 * Thimble engine - Needle 3 tool calling.
 *
 * Runs Cactus Compute's Needle 3 (Apache-2.0) straight from its shipped needle3.cact: the
 * same CQ 2/4-bit weights, A8 activations and int8 KV cache the archive declares. Text goes
 * in with a set of tools (JSON schemas); a call comes out, decoded under a grammar compiled
 * from those schemas, so it is always well formed: a known tool, known argument names, enum
 * values from the list, numbers inside their bounds, required arguments present. An empty
 * list is the model's refusal.
 *
 * The model is read in place (give thim_needle_open the bytes and keep them alive). Memory
 * comes from the thim_stt allocator hooks' twin below; no files, no threads, no libm.
 */
#ifndef THIMBLE_NEEDLE_H
#define THIMBLE_NEEDLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct thim_needle thim_needle;

void thim_needle_set_alloc(void *(*alloc)(size_t), void (*release)(void *));
void thim_needle_set_clock(double (*now_us)(void));

/* The model must stay in memory (8-byte aligned) until thim_needle_close. NULL + message on failure. */
thim_needle *thim_needle_open(const void *cact, size_t len, char *err, size_t err_len);
void         thim_needle_close(thim_needle *n);

/* Declares the toolset (a JSON array of {name, description, parameters} schemas, written
 * exactly as it should appear in the prompt) and an optional system text, and runs that
 * fixed part of the prompt once. Every later call reuses it. 0, or -1 with a message. */
int thim_needle_set_tools(thim_needle *n, const char *tools_json, const char *system, char *err, size_t err_len);

typedef struct {
    int max_new_tokens;     /* 0: 160 */
    int max_think_tokens;   /* 0: 64; the reasoning line is cut (closed) after this many */
    int no_refusal;         /* nonzero: [] is not allowed (a "trigger" matched) */
    int local_window;       /* attention width of the local layers (0: the archive's) */
    int gates;              /* THIM_GATE_* bits: checks of the call against the request (Cactus's grounding gates) */
    float floor;            /* withhold a call whose confidence is below this (0: 0.1, the engine floor) */
} thim_needle_opts;

/* Grounding gates. A gate that fires withholds the call (res.withheld says which); the call
 * itself is still written out so a caller can show it and ask. */
#define THIM_GATE_NEGATION 1    /* the request says don't / do not / never / not */
#define THIM_GATE_NUMBERS  2    /* every number argument is written in the request (digits or words) */
#define THIM_GATE_TEXT     4    /* every word of a free-text argument is in the request */
#define THIM_GATE_ENUM     8    /* an optional enum value the request never names is dropped; a required one withholds */
#define THIM_GATE_ALL      15
enum { THIM_OK = 0, THIM_WITHHELD_CONFIDENCE, THIM_WITHHELD_NEGATION, THIM_WITHHELD_NUMBER, THIM_WITHHELD_TEXT, THIM_WITHHELD_ENUM };

typedef struct {
    float confidence;       /* min(head, decode); -1 when the archive has no head */
    float head;             /* the calibrated head on prompt + call */
    float decode;           /* probability of the call's tokens */
    int   n_calls;          /* calls in `calls` ([] -> 0) */
    int   withheld;         /* THIM_OK, or why the call must not run (THIM_WITHHELD_*) */
    int   prompt_tokens, new_tokens;
    double prefill_us, decode_us;
} thim_needle_result;

/* One request against the declared tools. `calls` gets the call list as compact JSON
 * ([{"name":..,"arguments":{..}},..]); `reasoning` (optional) the model's reasoning line.
 * Returns 0, or -1 on error (message in calls). */
int thim_needle_run(thim_needle *n, const char *query, const thim_needle_opts *opts,
                    char *calls, size_t calls_cap, char *reasoning, size_t reasoning_cap,
                    thim_needle_result *res);

/* Low-level, for tests: tokenize; feed tokens and get the last position's logits (vocab floats). */
int  thim_needle_encode(thim_needle *n, const char *text, int *ids, int cap);
int  thim_needle_vocab(const thim_needle *n);
const char *thim_needle_piece(const thim_needle *n, int id);
void thim_needle_reset(thim_needle *n);
int  thim_needle_feed(thim_needle *n, const int *ids, int count, float *logits);
float thim_needle_head(thim_needle *n);

#ifdef __cplusplus
}
#endif
#endif
