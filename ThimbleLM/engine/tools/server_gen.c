/* /v1/chat/completions and /v1/completions:
 *   request parsing -> prompt (chat template, default system prompt, history
 *   trimming) -> prompt-cache prefill -> sampling loop (stop strings, think
 *   split, logprobs, n choices) -> JSON or SSE response. */
#include "server_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_STOPS     16
#define MAX_TOP_LP    20
#define MAX_BIAS      256

double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- request ------------------------------------------------------------------ */

typedef struct {
    thimble_sampling sampling;
    int    bias_ids[MAX_BIAS + 1];
    float  bias_vals[MAX_BIAS + 1];
    int    max_new, n, stream, include_usage, thinking, show_reasoning, cache_prompt, truncate;
    int    logprobs, top_logprobs;
    const char *stops[MAX_STOPS];
    int    n_stops;
} gen_request;

static int parse_request(const srv *s, const srv_model *sm, const json *b, int chat, gen_request *r,
                         char *err, size_t err_len) {
    const server_config *c = s->cfg;
    memset(r, 0, sizeof(*r));
    r->sampling = c->sampling;
#define NUM(key, def) json_num(json_get(b, key), def)
    r->sampling.temperature        = (float) NUM("temperature", c->sampling.temperature);
    r->sampling.top_p              = (float) NUM("top_p", c->sampling.top_p);
    r->sampling.top_k              = (int) NUM("top_k", c->sampling.top_k);
    r->sampling.min_p              = (float) NUM("min_p", c->sampling.min_p);
    r->sampling.repetition_penalty = (float) NUM("repetition_penalty", NUM("repeat_penalty", c->sampling.repetition_penalty));
    r->sampling.frequency_penalty  = (float) NUM("frequency_penalty", c->sampling.frequency_penalty);
    r->sampling.presence_penalty   = (float) NUM("presence_penalty", c->sampling.presence_penalty);
    double seed = NUM("seed", -1);
    r->sampling.seed = seed >= 0 ? (uint64_t) seed + 1 : ((uint64_t) time(NULL) << 20) ^ (uint64_t) clock() ^ (uint64_t) (uintptr_t) r;
    r->max_new = (int) NUM("max_tokens", NUM("max_completion_tokens", c->max_tokens));
    r->n       = (int) NUM("n", 1);
#undef NUM
    if (r->sampling.temperature < 0 || r->sampling.top_p <= 0 || r->sampling.top_p > 1) {
        snprintf(err, err_len, "temperature must be >= 0 and top_p in (0, 1]");
        return -1;
    }
    if (r->max_new < 1) { snprintf(err, err_len, "max_tokens must be >= 1"); return -1; }
    if (c->max_tokens_limit > 0 && r->max_new > c->max_tokens_limit) r->max_new = c->max_tokens_limit;
    if (r->n < 1 || r->n > c->max_n) { snprintf(err, err_len, "n must be between 1 and %d", c->max_n); return -1; }

    r->stream        = json_bool(json_get(b, "stream"), 0);
    r->include_usage = json_bool(json_get(json_get(b, "stream_options"), "include_usage"), 0);
    r->cache_prompt  = json_bool(json_get(b, "cache_prompt"), c->prompt_cache);
    r->truncate      = json_bool(json_get(b, "truncate"), 0);
    r->show_reasoning = json_bool(json_get(b, "include_reasoning"), c->show_reasoning);

    const json *think = json_get(b, "enable_thinking");
    if (!think || think->type != J_BOOL) think = json_get(json_get(b, "chat_template_kwargs"), "enable_thinking");
    const json *effort = json_get(b, "reasoning_effort");   /* OpenAI style: "none" disables */
    if (think && think->type == J_BOOL) r->thinking = think->boolean ? THIMBLE_THINK_ON : THIMBLE_THINK_OFF;
    else if (effort && effort->type == J_STR) r->thinking = strcmp(effort->str, "none") ? THIMBLE_THINK_ON : THIMBLE_THINK_OFF;
    else r->thinking = c->thinking;
    if (r->thinking != THIMBLE_THINK_DEFAULT && !thimble_has_thinking(sm->model)) {
        if (r->thinking == THIMBLE_THINK_ON && (think || effort)) {
            snprintf(err, err_len, "model '%s' was not trained with think mode", sm->alias);
            return -1;
        }
        r->thinking = THIMBLE_THINK_DEFAULT;   /* server default does not apply to plain models */
    }

    /* logprobs: chat uses a bool + top_logprobs, legacy completions an int */
    const json *lp = json_get(b, "logprobs");
    if (lp && lp->type == J_BOOL) r->logprobs = lp->boolean, r->top_logprobs = (int) json_num(json_get(b, "top_logprobs"), 0);
    else if (lp && lp->type == J_NUM) r->logprobs = 1, r->top_logprobs = (int) lp->num;
    if (r->top_logprobs < 0 || r->top_logprobs > MAX_TOP_LP) { snprintf(err, err_len, "top_logprobs must be 0..%d", MAX_TOP_LP); return -1; }

    const json *stop = json_get(b, "stop");
    if (stop && stop->type == J_STR && stop->str[0]) r->stops[r->n_stops++] = stop->str;
    if (stop && stop->type == J_ARR)
        for (const json *x = stop->child; x && r->n_stops < MAX_STOPS; x = x->next)
            if (x->type == J_STR && x->str[0]) r->stops[r->n_stops++] = x->str;

    int nb = 0, nv = thimble_n_vocab(sm->model);
    const json *bias = json_get(b, "logit_bias");   /* {"token id": bias} */
    for (const json *x = bias && bias->type == J_OBJ ? bias->child : NULL; x && nb < MAX_BIAS; x = x->next) {
        char *end;
        long id = strtol(x->key, &end, 10);
        if (*end || id < 0 || id >= nv || x->type != J_NUM) { snprintf(err, err_len, "bad logit_bias entry '%s'", x->key); return -1; }
        r->bias_ids[nb] = (int) id, r->bias_vals[nb++] = (float) x->num;
    }
    if (json_bool(json_get(b, "ignore_eos"), 0))
        r->bias_ids[nb] = thimble_eos_id(sm->model), r->bias_vals[nb++] = -1e30f;
    r->sampling.logit_bias_ids = r->bias_ids;
    r->sampling.logit_bias     = r->bias_vals;
    r->sampling.n_logit_bias   = nb;
    (void) chat;
    return 0;
}

/* ---- prompt building -------------------------------------------------------------- */

static int *tokenize(const thimble_model *m, const char *text, int *n) {
    *n = thimble_tokenize(m, text, NULL, 0);
    int *ids = malloc(sizeof(int) * (size_t) (*n + 1));
    thimble_tokenize(m, text, ids, *n);
    return ids;
}

/* Message content may be a string or OpenAI content parts [{type:"text",text:..}]. */
static char *content_text(const json *content) {
    if (content && content->type == J_STR) return strdup(content->str);
    if (!content || content->type != J_ARR) return NULL;
    sbuf b;
    sb_init(&b);
    for (const json *p = content->child; p; p = p->next) {
        const char *t = json_str(json_get(p, "text"), NULL);
        if (t) sb_puts(&b, t);
    }
    if (!b.s) sb_puts(&b, "");
    return b.s;
}

static int *build_chat_prompt(const srv *s, const srv_model *sm, const json *msgs, const gen_request *r,
                              int *n_prompt, int *dropped, char *err, size_t err_len) {
    int n = 0;
    for (const json *m = msgs->child; m; m = m->next) n++;
    thimble_message *list = calloc((size_t) n + 1, sizeof(thimble_message));
    char **owned = calloc((size_t) n + 1, sizeof(char *));
    int k = 0, has_system = 0, *ids = NULL;
    for (const json *m = msgs->child; m; m = m->next) {
        const char *role = json_str(json_get(m, "role"), NULL);
        char *text = content_text(json_get(m, "content"));
        if (role && !strcmp(role, "developer")) role = "system";   /* newer OpenAI name */
        if (!role || !text || (strcmp(role, "system") && strcmp(role, "user") && strcmp(role, "assistant"))) {
            snprintf(err, err_len, "each message needs role system|developer|user|assistant and text content");
            free(text);
            goto done;
        }
        has_system |= !strcmp(role, "system");
        owned[k] = text;
        list[k++] = (thimble_message) { role, text };
    }
    if (!has_system && s->cfg->system_prompt[0]) {   /* default system prompt goes first */
        memmove(list + 1, list, sizeof(thimble_message) * (size_t) k);
        list[0] = (thimble_message) { "system", s->cfg->system_prompt };
        k++;
    }

    /* drop the oldest non-system turns until the prompt leaves room for an answer */
    int first = 0, reserve = r->max_new < s->cfg->min_gen ? r->max_new : s->cfg->min_gen;
    while (list[first].role && !strcmp(list[first].role, "system") && first < k - 1) first++;
    for (;;) {
        char *prompt = thimble_render_chat(sm->model, list, k, r->thinking);
        if (!prompt) { snprintf(err, err_len, "cannot render chat (think mode unsupported?)"); goto done; }
        free(ids);
        ids = tokenize(sm->model, prompt, n_prompt);
        free(prompt);
        if (*n_prompt + reserve <= sm->n_ctx) break;
        if (!s->cfg->trim_history || k - first <= 1) {
            snprintf(err, err_len, "prompt is %d tokens; the context is %d (answer room %d)", *n_prompt, sm->n_ctx, reserve);
            free(ids);
            ids = NULL;
            goto done;
        }
        int drop = 1;   /* also drop the reply of a dropped user turn so history starts at a user turn */
        if (first + 1 < k - 1 && !strcmp(list[first].role, "user") && !strcmp(list[first + 1].role, "assistant")) drop = 2;
        memmove(list + first, list + first + drop, sizeof(thimble_message) * (size_t) (k - first - drop));
        k -= drop;
        *dropped += drop;
    }
done:
    for (int i = 0; i < n; i++) free(owned[i]);
    free(owned);
    free(list);
    return ids;
}

/* ---- prompt cache + prefill ---------------------------------------------------------- */

static const float *prefill(srv_slot *slot, const int *ids, int n, int use_cache, int *reused) {
    int common = 0;
    if (use_cache)
        while (common < slot->n_cached && common < n && slot->cached[common] == ids[common]) common++;
    int keep = common < n - 1 ? common : n - 1;   /* re-evaluate at least the last token for its logits */
    if (thimble_ctx_truncate(slot->ctx, keep) != 0) {   /* every layer type can rewind */
        thimble_ctx_reset(slot->ctx);
        keep = 0;
    }
    slot->n_cached = keep;
    *reused = keep;
    const float *logits = NULL;
    for (int i = keep; i < n; i++) {
        logits = thimble_eval(slot->ctx, ids[i]);
        if (!logits) return NULL;
        slot->cached[slot->n_cached++] = ids[i];
    }
    return logits;
}

/* ---- streaming helpers ------------------------------------------------------------------ */

/* bytes at the end of s that might be the start of a stop string */
static size_t stop_overlap(const char *s, size_t n, const gen_request *r) {
    size_t best = 0;
    for (int i = 0; i < r->n_stops; i++) {
        size_t sl = strlen(r->stops[i]);
        for (size_t k = sl - 1; k > best; k--)
            if (k <= n && !memcmp(s + n - k, r->stops[i], k)) { best = k; break; }
    }
    return best;
}

/* bytes at the end of s that form an incomplete UTF-8 sequence */
static size_t utf8_tail(const char *s, size_t n) {
    for (size_t k = 1; k <= 3 && k <= n; k++) {
        unsigned char c = (unsigned char) s[n - k];
        if ((c & 0xC0) == 0x80) continue;   /* continuation byte */
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > k ? k : 0;
    }
    return 0;
}

typedef struct {
    pthread_mutex_t lock;     /* serializes SSE writes from parallel choices */
    int             io_error; /* client gone: every choice stops */
} out_shared;

typedef struct {
    const srv       *s;
    const srv_model *sm;
    http_request    *req;
    const gen_request *r;
    int    chat, index;
    char   id[40];
    long   created;
    out_shared *sh;
} out_ctx;

static void chunk_begin(const out_ctx *o, sbuf *b) {
    sb_printf(b, "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%ld,\"model\":", o->id,
              o->chat ? "chat.completion.chunk" : "text_completion", o->created);
    sb_json_str(b, o->sm->alias, strlen(o->sm->alias));
    sb_printf(b, ",\"choices\":[{\"index\":%d,", o->index);
}

static void send_event(out_ctx *o, sbuf *b) {
    pthread_mutex_lock(&o->sh->lock);
    if (!o->sh->io_error && http_sse_event(o->req, b->s, b->n)) o->sh->io_error = 1;
    pthread_mutex_unlock(&o->sh->lock);
    sb_free(b);
}

static void emit_delta(out_ctx *o, const char *field, const char *text, size_t len, const char *lp_json) {
    sbuf b;
    sb_init(&b);
    chunk_begin(o, &b);
    if (o->chat) {
        sb_puts(&b, "\"delta\":{");
        if (field) { sb_printf(&b, "\"%s\":", field); sb_json_str(&b, text, len); }
        sb_puts(&b, "}");
    } else {
        sb_puts(&b, "\"text\":");
        sb_json_str(&b, text ? text : "", text ? len : 0);
    }
    if (lp_json) { sb_puts(&b, ",\"logprobs\":"); sb_puts(&b, lp_json); }
    sb_puts(&b, ",\"finish_reason\":null}]}");
    send_event(o, &b);
}

/* ---- logprobs ------------------------------------------------------------------------- */

static void piece_json(const thimble_model *m, int id, sbuf *b, int with_bytes) {
    char buf[256];
    int  n = thimble_token_piece(m, id, buf, (int) sizeof(buf), 1);
    if (n > (int) sizeof(buf)) n = (int) sizeof(buf);
    sb_puts(b, "\"token\":");
    sb_json_str(b, buf, (size_t) n);
    if (with_bytes) {
        sb_puts(b, ",\"bytes\":[");
        for (int i = 0; i < n; i++) sb_printf(b, i ? ",%d" : "%d", (unsigned char) buf[i]);
        sb_puts(b, "]");
    }
}

/* One OpenAI chat logprobs entry {token, logprob, bytes, top_logprobs[...]} */
static void logprob_entry(const thimble_model *m, const float *logits, int tok, int top, sbuf *b) {
    const int nv = thimble_n_vocab(m);
    float mx = logits[0];
    for (int i = 1; i < nv; i++) if (logits[i] > mx) mx = logits[i];
    double sum = 0;
    for (int i = 0; i < nv; i++) sum += exp((double) (logits[i] - mx));
    const double lse = mx + log(sum);
    sb_puts(b, "{");
    piece_json(m, tok, b, 1);
    sb_printf(b, ",\"logprob\":%.6f,\"top_logprobs\":[", logits[tok] - lse);
    int chosen[MAX_TOP_LP];
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int i = 0; i < nv; i++) {
            int taken = 0;
            for (int j = 0; j < k; j++) taken |= chosen[j] == i;
            if (!taken && (best < 0 || logits[i] > logits[best])) best = i;
        }
        chosen[k] = best;
        sb_puts(b, k ? ",{" : "{");
        piece_json(m, best, b, 1);
        sb_printf(b, ",\"logprob\":%.6f}", logits[best] - lse);
    }
    sb_puts(b, "]}");
}

/* ---- one choice ------------------------------------------------------------------------------ */

typedef struct {
    sbuf   answer, reasoning, logprobs;   /* logprobs: comma-separated entries */
    const char *finish;
    int    n_tokens, reused, n_prompt;
    double prompt_s, gen_s;
} choice_result;

/* Generate one choice on ctx, which already holds the prompt; start_logits are
 * the prompt's last logits. slot (choice 0 only) records fed tokens for the cache. */
static int run_choice(out_ctx *o, thimble_ctx *ctx, srv_slot *slot, const float *start_logits,
                      const int *ids, int n_prompt, choice_result *cr) {
    const thimble_model *m = o->sm->model;
    const gen_request   *r = o->r;
    const thimble_special sp = thimble_special_tokens(m);
    sb_init(&cr->answer), sb_init(&cr->reasoning), sb_init(&cr->logprobs);
    cr->n_prompt = n_prompt;
    cr->finish   = "length";
    const float *logits = start_logits;
    double t0;

    int room    = thimble_ctx_n_ctx(ctx) - n_prompt;
    int max_new = r->max_new < room + 1 ? r->max_new : room + 1;   /* the last token need not be fed */
    int reasoning = sp.think >= 0 && ids[n_prompt - 1] == sp.think;
    int *history = malloc(sizeof(int) * (size_t) (max_new + 1));
    uint64_t rng = r->sampling.seed + (uint64_t) o->index * 0x9E3779B97F4A7C15ull;
    size_t emitted = 0, r_emitted = 0;
    char piece[256];

    t0 = now_seconds();
    for (int step = 0; step < max_new && !o->sh->io_error; step++) {
        int tok = thimble_sample(m, logits, &r->sampling, &rng, history, cr->n_tokens);
        if (tok == sp.eos) { cr->finish = "stop"; break; }
        history[cr->n_tokens++] = tok;
        int stop_hit = 0;
        if (reasoning && tok == sp.think_end) {
            reasoning = 0;
        } else {
            int len = thimble_token_piece(m, tok, piece, (int) sizeof(piece), 0);
            if (len > (int) sizeof(piece)) len = (int) sizeof(piece);
            if (reasoning) {
                sb_putn(&cr->reasoning, piece, (size_t) len);
                if (r->stream && r->show_reasoning && o->chat) {
                    size_t safe = cr->reasoning.n - utf8_tail(cr->reasoning.s, cr->reasoning.n);
                    if (safe > r_emitted) emit_delta(o, "reasoning_content", cr->reasoning.s + r_emitted, safe - r_emitted, NULL);
                    if (safe > r_emitted) r_emitted = safe;
                }
            } else {
                size_t before = cr->answer.n;
                sb_putn(&cr->answer, piece, (size_t) len);
                if (!cr->answer.s) sb_puts(&cr->answer, "");
                size_t cut = cr->answer.n;
                for (int i = 0; i < r->n_stops; i++) {
                    size_t sl = strlen(r->stops[i]), from = before >= sl ? before - sl + 1 : 0;
                    char *hit = strstr(cr->answer.s + from, r->stops[i]);
                    if (hit && (size_t) (hit - cr->answer.s) < cut) cut = (size_t) (hit - cr->answer.s);
                }
                if (cut < cr->answer.n) cr->answer.n = cut, cr->answer.s[cut] = '\0', stop_hit = 1;
                sbuf lp;
                sb_init(&lp);
                if (r->logprobs) {
                    logprob_entry(m, logits, tok, r->top_logprobs, &lp);
                    if (cr->logprobs.n) sb_puts(&cr->logprobs, ",");
                    sb_putn(&cr->logprobs, lp.s, lp.n);
                }
                if (r->stream) {   /* hold back a possible stop-string prefix and split UTF-8 */
                    size_t safe = cr->answer.n;
                    if (!stop_hit) {
                        size_t hold = stop_overlap(cr->answer.s, cr->answer.n, r);
                        size_t u    = utf8_tail(cr->answer.s, cr->answer.n);
                        safe -= hold > u ? hold : u;
                    }
                    char *lpj = NULL;
                    if (r->logprobs) {
                        sbuf w;
                        sb_init(&w);
                        sb_puts(&w, o->chat ? "{\"content\":[" : "{\"content\":[");
                        sb_putn(&w, lp.s, lp.n);
                        sb_puts(&w, "]}");
                        lpj = w.s;
                    }
                    if (safe > emitted || lpj)
                        emit_delta(o, "content", cr->answer.s + emitted, safe > emitted ? safe - emitted : 0, lpj);
                    if (safe > emitted) emitted = safe;
                    free(lpj);
                }
                sb_free(&lp);
            }
        }
        if (stop_hit) { cr->finish = "stop"; break; }
        if (step + 1 < max_new) {
            logits = thimble_eval(ctx, tok);
            if (!logits) break;
            if (slot) slot->cached[slot->n_cached++] = tok;
        }
    }
    cr->gen_s = now_seconds() - t0;
    free(history);
    if (r->stream && cr->answer.n > emitted) emit_delta(o, "content", cr->answer.s + emitted, cr->answer.n - emitted, NULL);
    if (r->stream && o->chat && r->show_reasoning && cr->reasoning.n > r_emitted)
        emit_delta(o, "reasoning_content", cr->reasoning.s + r_emitted, cr->reasoning.n - r_emitted, NULL);
    if (r->stream) {   /* finish chunk for this choice */
        sbuf b;
        sb_init(&b);
        chunk_begin(o, &b);
        sb_puts(&b, o->chat ? "\"delta\":{}" : "\"text\":\"\"");
        sb_printf(&b, ",\"finish_reason\":\"%s\"}]}", cr->finish);
        send_event(o, &b);
    }
    return 0;
}

typedef struct {
    out_ctx        o;
    thimble_ctx   *ctx;
    srv_slot      *slot;
    const float   *logits;
    const int     *ids;
    int            n_prompt, rc;
    choice_result *cr;
} choice_job;

static void *choice_thread(void *arg) {
    choice_job *j = arg;
    j->rc = run_choice(&j->o, j->ctx, j->slot, j->logits, j->ids, j->n_prompt, j->cr);
    return NULL;
}

static void free_choice(choice_result *cr) {
    sb_free(&cr->answer), sb_free(&cr->reasoning), sb_free(&cr->logprobs);
}

static void trimmed(sbuf *b, const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\n')) s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n')) n--;
    sb_json_str(b, s ? s : "", n);
}

static void usage_json(sbuf *b, int n_prompt, int reused, int completion, int dropped, double prompt_s, double gen_s) {
    sb_printf(b, "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"total_tokens\":%d,"
                 "\"prompt_tokens_details\":{\"cached_tokens\":%d}},"
                 "\"timings\":{\"prompt_ms\":%.2f,\"predicted_ms\":%.2f,\"predicted_per_second\":%.1f,"
                 "\"dropped_messages\":%d}",
              n_prompt, completion, n_prompt + completion, reused, prompt_s * 1e3, gen_s * 1e3,
              gen_s > 0 ? completion / gen_s : 0.0, dropped);
}

/* ---- entry point ------------------------------------------------------------------------------ */

const srv_model *srv_pick_model(const srv *s, const json *body, char *err, size_t err_len) {
    const char *want = json_str(json_get(body, "model"), NULL);
    if (!want || !want[0]) return &s->models[0];
    for (int i = 0; i < s->n_models; i++)
        if (!strcmp(s->models[i].alias, want)) return &s->models[i];
    if (s->n_models == 1) return &s->models[0];   /* single model: accept any name (OpenAI clients) */
    snprintf(err, err_len, "model '%s' not found; available:", want);
    for (int i = 0; i < s->n_models; i++) {
        size_t l = strlen(err);
        snprintf(err + l, err_len - l, " %s", s->models[i].alias);
    }
    return NULL;
}

void srv_handle_generation(srv *s, http_request *req, const json *body, int chat) {
    char err[512];
    const srv_model *sm = srv_pick_model(s, body, err, sizeof(err));
    if (!sm) { http_send_error(req, 404, err); return; }
    gen_request r;
    if (parse_request(s, sm, body, chat, &r, err, sizeof(err))) { http_send_error(req, 400, err); return; }

    int n_prompt = 0, dropped = 0, *ids = NULL;
    if (chat) {
        const json *msgs = json_get(body, "messages");
        if (!msgs || msgs->type != J_ARR || !msgs->child) { http_send_error(req, 400, "messages must be a non-empty array"); return; }
        ids = build_chat_prompt(s, sm, msgs, &r, &n_prompt, &dropped, err, sizeof(err));
        if (!ids) { http_send_error(req, 400, err); return; }
    } else {
        const json *pr = json_get(body, "prompt");
        if (pr && pr->type == J_ARR) pr = pr->child;
        if (!pr || pr->type != J_STR) { http_send_error(req, 400, "prompt must be a string"); return; }
        ids = tokenize(sm->model, pr->str, &n_prompt);
        int limit = sm->n_ctx - 1;
        if (n_prompt > limit) {
            if (!r.truncate) {
                snprintf(err, sizeof(err), "prompt is %d tokens; the context is %d (send \"truncate\": true to keep the end)", n_prompt, sm->n_ctx);
                free(ids);
                http_send_error(req, 400, err);
                return;
            }
            memmove(ids, ids + n_prompt - limit, sizeof(int) * (size_t) limit);
            n_prompt = limit;
        }
    }
    if (n_prompt == 0) { free(ids); http_send_error(req, 400, "empty prompt"); return; }

    out_shared sh = { PTHREAD_MUTEX_INITIALIZER, 0 };
    out_ctx o = { s, sm, req, &r, chat, 0, "", (long) time(NULL), &sh };
    pthread_mutex_lock((pthread_mutex_t *) &s->m.mu);
    snprintf(o.id, sizeof(o.id), "%s-%llx", chat ? "chatcmpl" : "cmpl", (unsigned long long) ++((srv *) s)->m.next_id);
    pthread_mutex_unlock((pthread_mutex_t *) &s->m.mu);

    srv_slot *slot = &sm->slots[req->worker];
    choice_result *res = calloc((size_t) r.n, sizeof(choice_result));
    int total_completion = 0, failed = 0, reused = 0;
    double ps = 0, gs = 0;

    /* evaluate the prompt once (with the cache), then fork it for each extra choice */
    double t0 = now_seconds();
    const float *last = prefill(slot, ids, n_prompt, r.cache_prompt, &reused);
    ps = now_seconds() - t0;
    float *start = NULL;
    if (!last) failed = 1;
    else {
        start = malloc(sizeof(float) * (size_t) thimble_n_vocab(sm->model));
        memcpy(start, last, sizeof(float) * (size_t) thimble_n_vocab(sm->model));
    }
    while (!failed && slot->n_forks < r.n - 1) {
        slot->forks = realloc(slot->forks, sizeof(thimble_ctx *) * (size_t) (slot->n_forks + 1));
        slot->forks[slot->n_forks] = thimble_ctx_new(sm->model, sm->n_ctx);
        if (!slot->forks[slot->n_forks]) failed = 1;
        else slot->n_forks++;
    }

    if (r.stream) {
        http_sse_begin(req);
        if (chat) for (int i = 0; i < r.n; i++) { o.index = i; emit_delta(&o, "role", "assistant", 9, NULL); }
    }
    if (!failed) {
        choice_job *jobs = calloc((size_t) r.n, sizeof(choice_job));
        pthread_t  *th   = calloc((size_t) r.n, sizeof(pthread_t));
        double g0 = now_seconds();
        for (int i = 0; i < r.n; i++) {
            jobs[i] = (choice_job) { o, i ? slot->forks[i - 1] : slot->ctx, i ? NULL : slot, start, ids, n_prompt, 0, &res[i] };
            jobs[i].o.index = i;
            res[i].reused = reused;
            if (i) thimble_ctx_copy(jobs[i].ctx, slot->ctx);   /* fork before choice 0 moves on */
        }
        for (int i = 1; i < r.n; i++) pthread_create(&th[i], NULL, choice_thread, &jobs[i]);
        choice_thread(&jobs[0]);   /* choice 0 on this thread, extending the cached slot */
        for (int i = 1; i < r.n; i++) pthread_join(th[i], NULL);
        gs = now_seconds() - g0;
        for (int i = 0; i < r.n; i++) {
            failed |= jobs[i].rc != 0;
            total_completion += res[i].n_tokens;
        }
        free(jobs);
        free(th);
    }
    free(start);

    if (r.stream) {
        if (r.include_usage && !failed) {
            sbuf b;
            sb_init(&b);
            sb_printf(&b, "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%ld,\"model\":", o.id,
                      chat ? "chat.completion.chunk" : "text_completion", o.created);
            sb_json_str(&b, sm->alias, strlen(sm->alias));
            sb_puts(&b, ",\"choices\":[],");
            usage_json(&b, n_prompt, reused, total_completion, dropped, ps, gs);
            sb_puts(&b, "}");
            send_event(&o, &b);
        }
        if (failed) {
            const char *e = "{\"error\":{\"message\":\"generation failed\",\"type\":\"server_error\"}}";
            http_sse_event(req, e, strlen(e));
        }
        http_sse_event(req, "[DONE]", 6);
    } else if (failed) {
        http_send_error(req, 500, "generation failed");
    } else {
        sbuf b;
        sb_init(&b);
        sb_printf(&b, "{\"id\":\"%s\",\"object\":\"%s\",\"created\":%ld,\"model\":", o.id,
                  chat ? "chat.completion" : "text_completion", o.created);
        sb_json_str(&b, sm->alias, strlen(sm->alias));
        sb_puts(&b, ",\"choices\":[");
        for (int i = 0; i < r.n; i++) {
            choice_result *cr = &res[i];
            sb_printf(&b, "%s{\"index\":%d,", i ? "," : "", i);
            if (chat) {
                sb_puts(&b, "\"message\":{\"role\":\"assistant\",\"content\":");
                trimmed(&b, cr->answer.s, cr->answer.n);
                if (r.show_reasoning && cr->reasoning.n) { sb_puts(&b, ",\"reasoning_content\":"); trimmed(&b, cr->reasoning.s, cr->reasoning.n); }
                sb_puts(&b, "}");
            } else {
                sb_puts(&b, "\"text\":");
                sb_json_str(&b, cr->answer.s ? cr->answer.s : "", cr->answer.n);
            }
            if (r.logprobs) {
                sb_puts(&b, ",\"logprobs\":{\"content\":[");
                if (cr->logprobs.n) sb_putn(&b, cr->logprobs.s, cr->logprobs.n);
                sb_puts(&b, "]}");
            } else {
                sb_puts(&b, ",\"logprobs\":null");
            }
            sb_printf(&b, ",\"finish_reason\":\"%s\"}", cr->finish);
        }
        sb_puts(&b, "],");
        usage_json(&b, n_prompt, reused, total_completion, dropped, ps, gs);
        sb_puts(&b, "}");
        http_send_json(req, 200, b.s);
        sb_free(&b);
    }

    pthread_mutex_lock((pthread_mutex_t *) &s->m.mu);
    srv_metrics *mt = (srv_metrics *) &s->m;
    mt->generations += (uint64_t) r.n;
    mt->prompt_tokens += (uint64_t) n_prompt;
    mt->cached_tokens += (uint64_t) reused;
    mt->completion_tokens += (uint64_t) total_completion;
    mt->dropped_messages += (uint64_t) dropped;
    mt->prompt_seconds += ps;
    mt->gen_seconds += gs;
    if (failed) mt->errors++;
    pthread_mutex_unlock((pthread_mutex_t *) &s->m.mu);

    for (int i = 0; i < r.n; i++) free_choice(&res[i]);
    free(res);
    free(ids);
}
