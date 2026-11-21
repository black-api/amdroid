/* Server settings: defaults < JSON config file (--config) < command-line flags. */
#ifndef THIMBLE_SERVER_CONFIG_H
#define THIMBLE_SERVER_CONFIG_H

#include "thimble.h"

#define MAX_MODELS 8

typedef struct {
    const char *path;
    char        alias[128];   /* name clients send as "model"; defaults to general.name */
} model_spec;

typedef struct {
    /* network */
    char   host[128];
    int    port;
    int    parallel;          /* worker threads = concurrent requests */
    int    queue_size;
    int    keepalive_s;
    int    timeout_s;
    size_t max_body;
    char   cors[256];         /* "" disables CORS headers */
    char   api_key[256];      /* "" disables auth (also THIMBLE_API_KEY) */
    int    webui;
    int    metrics;
    int    log_level;         /* 0 quiet, 1 requests, 2 verbose */
    int    voice_threads;     /* 0 = default (cores, max 8) */
    char   voice[512];        /* voice file or name; "" = find one in the voice cache; "off" = no speech */

    /* models */
    model_spec models[MAX_MODELS];
    int        n_models;
    int        n_ctx;         /* <= 0: training context */

    /* generation defaults (requests may override unless locked) */
    thimble_sampling sampling;
    int    max_tokens;        /* default when the request has none */
    int    max_tokens_limit;  /* hard cap; 0 = up to the remaining context */
    int    thinking;          /* THIMBLE_THINK_* default for chat */
    int    show_reasoning;    /* include reasoning_content in responses */
    char   system_prompt[4096];  /* prepended when a chat has no system message */
    int    prompt_cache;      /* reuse KV cache across requests on a worker */
    int    trim_history;      /* drop oldest chat turns that do not fit */
    int    min_gen;           /* tokens kept free for the answer when trimming */
    int    max_n;             /* cap for the "n" (choices) parameter */
} server_config;

void server_config_defaults(server_config *c);

/* Parse "--flag value" arguments (argv excludes the program/command); model
 * paths may be given positionally or with --model PATH[=ALIAS].
 * Returns 0, or -1 with a message in err. */
int server_config_parse(server_config *c, int argc, char **argv, char *err, size_t err_len);

/* Load a JSON config file (same keys as the long flags, with '-' -> '_'). */
int server_config_load_file(server_config *c, const char *path, char *err, size_t err_len);

void server_config_usage(void);

/* JSON dump of the effective settings (api_key redacted). Caller frees. */
char *server_config_json(const server_config *c);

#endif
