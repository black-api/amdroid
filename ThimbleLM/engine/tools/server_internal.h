/* Shared server state (server.c: routing/endpoints, server_gen.c: generation). */
#ifndef THIMBLE_SERVER_INTERNAL_H
#define THIMBLE_SERVER_INTERNAL_H

#include "http.h"
#include "json.h"
#include "server_config.h"
#include "thimble.h"

#include <pthread.h>
#include <stdint.h>

/* One inference slot per (worker thread, model): a context plus the tokens it
 * currently holds, which is what the prompt cache matches against. */
typedef struct {
    thimble_ctx  *ctx;
    int          *cached;
    int           n_cached;
    thimble_ctx **forks;      /* extra contexts for parallel choices (n > 1), created on demand */
    int           n_forks;
} srv_slot;

typedef struct {
    thimble_model *model;
    char           alias[128];
    int            n_ctx;
    srv_slot      *slots;     /* [parallel] */
} srv_model;

typedef struct {
    pthread_mutex_t mu;
    uint64_t requests, errors, generations, prompt_tokens, cached_tokens, completion_tokens, dropped_messages;
    double   prompt_seconds, gen_seconds;
    int      active;
    uint64_t next_id;
} srv_metrics;

typedef struct {
    server_config *cfg;
    srv_model      models[MAX_MODELS];
    int            n_models;
    srv_metrics    m;
    thimble_voice *voice;     /* NULL when no voice is installed */
    char           voice_path[1024];
    pthread_mutex_t voice_mu; /* one synthesis at a time */
    long           started;
} srv;

/* server_gen.c */
void srv_handle_generation(srv *s, http_request *req, const json *body, int chat);
const srv_model *srv_pick_model(const srv *s, const json *body, char *err, size_t err_len);

/* server_voice.c */
void srv_handle_speech(srv *s, http_request *req, const json *body);
void srv_load_voice(srv *s);

/* helpers */
double now_seconds(void);

#endif
