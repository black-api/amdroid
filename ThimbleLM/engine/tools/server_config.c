#include "server_config.h"
#include "json.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void server_config_defaults(server_config *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "127.0.0.1");
    c->port         = 8080;
    c->parallel     = 2;
    c->queue_size   = 64;
    c->keepalive_s  = 5;
    c->timeout_s    = 30;
    c->max_body     = 8u << 20;
    snprintf(c->cors, sizeof(c->cors), "*");
    c->webui        = 1;
    c->metrics      = 1;
    c->log_level    = 1;
    c->sampling     = thimble_sampling_default();
    c->sampling.temperature = 0.8f;
    c->sampling.top_p       = 0.9f;
    c->max_tokens       = 256;
    c->thinking         = THIMBLE_THINK_DEFAULT;
    c->show_reasoning   = 1;
    c->prompt_cache     = 1;
    c->trim_history     = 1;
    c->min_gen          = 16;
    c->max_n            = 8;
    const char *key = getenv("THIMBLE_API_KEY");
    if (key) snprintf(c->api_key, sizeof(c->api_key), "%s", key);
}

/* ---- one table drives the CLI flags, the JSON config file and --help ------- */

typedef enum { O_INT, O_FLOAT, O_STR, O_BOOL, O_THINK, O_LOG, O_SIZE } opt_type;

typedef struct {
    const char *name;     /* long flag without "--"; JSON key uses '_' for '-' */
    opt_type    type;
    size_t      offset;
    size_t      size;     /* for O_STR */
    const char *help;
} option;

#define OFF(f) offsetof(server_config, f)
#define STR(f) OFF(f), sizeof(((server_config *) 0)->f)

static const option OPTIONS[] = {
    { "host",              O_STR,   STR(host),             "listen address (127.0.0.1; 0.0.0.0 for all)" },
    { "port",              O_INT,   OFF(port), 0,          "listen port (8080)" },
    { "parallel",          O_INT,   OFF(parallel), 0,      "worker threads / concurrent requests (2)" },
    { "queue",             O_INT,   OFF(queue_size), 0,    "pending connections before 503 (64)" },
    { "keepalive",         O_INT,   OFF(keepalive_s), 0,   "keep-alive idle seconds, 0 = off (5)" },
    { "timeout",           O_INT,   OFF(timeout_s), 0,     "request read timeout seconds (30)" },
    { "max-body",          O_SIZE,  OFF(max_body), 0,      "max request body bytes (8388608)" },
    { "cors",              O_STR,   STR(cors),             "Access-Control-Allow-Origin, empty = off (*)" },
    { "api-key",           O_STR,   STR(api_key),          "require 'Authorization: Bearer KEY' (env THIMBLE_API_KEY)" },
    { "webui",             O_BOOL,  OFF(webui), 0,         "serve the chat page at / (on)" },
    { "metrics",           O_BOOL,  OFF(metrics), 0,       "expose /metrics (on)" },
    { "voice-threads",     O_INT,   OFF(voice_threads), 0, "threads for speech synthesis, 0 = cores (0)" },
    { "voice",             O_STR,   STR(voice),            "voice file or name for /v1/audio/speech; default: first in ~/.cache/thimble/voices; off = none" },
    { "log",               O_LOG,   OFF(log_level), 0,     "quiet | requests | verbose (requests)" },
    { "ctx",               O_INT,   OFF(n_ctx), 0,         "context length, 0 = model's (0)" },
    { "temp",              O_FLOAT, OFF(sampling.temperature), 0,        "default temperature (0.8)" },
    { "top-k",             O_INT,   OFF(sampling.top_k), 0,              "default top-k, 0 = off (0)" },
    { "top-p",             O_FLOAT, OFF(sampling.top_p), 0,              "default top-p (0.9)" },
    { "min-p",             O_FLOAT, OFF(sampling.min_p), 0,              "default min-p (0)" },
    { "repeat-penalty",    O_FLOAT, OFF(sampling.repetition_penalty), 0, "default repetition penalty (1)" },
    { "frequency-penalty", O_FLOAT, OFF(sampling.frequency_penalty), 0,  "default frequency penalty (0)" },
    { "presence-penalty",  O_FLOAT, OFF(sampling.presence_penalty), 0,   "default presence penalty (0)" },
    { "max-tokens",        O_INT,   OFF(max_tokens), 0,       "default max_tokens (256)" },
    { "max-tokens-limit",  O_INT,   OFF(max_tokens_limit), 0, "hard cap on max_tokens, 0 = context (0)" },
    { "think",             O_THINK, OFF(thinking), 0,         "default think mode: default | on | off" },
    { "show-reasoning",    O_BOOL,  OFF(show_reasoning), 0,   "return reasoning_content (on)" },
    { "system-prompt",     O_STR,   STR(system_prompt),       "system message added when a chat has none" },
    { "prompt-cache",      O_BOOL,  OFF(prompt_cache), 0,     "reuse the KV cache across requests (on)" },
    { "trim-history",      O_BOOL,  OFF(trim_history), 0,     "drop oldest chat turns that do not fit (on)" },
    { "min-gen",           O_INT,   OFF(min_gen), 0,          "tokens kept free for the answer when trimming (16)" },
    { "max-n",             O_INT,   OFF(max_n), 0,            "max choices per request ('n') (8)" },
};

static const option *find_option(const char *name) {
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++) {
        const char *a = OPTIONS[i].name, *b = name;
        while (*a && (*a == *b || (*a == '-' && *b == '_'))) a++, b++;
        if (!*a && !*b) return &OPTIONS[i];
    }
    return NULL;
}

static int parse_bool(const char *v, int *out) {
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcasecmp(v, "yes")) { *out = 1; return 0; }
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcasecmp(v, "no")) { *out = 0; return 0; }
    return -1;
}

static int set_option(server_config *c, const option *o, const char *v, char *err, size_t err_len) {
    char *base = (char *) c + o->offset, *end = NULL;
    switch (o->type) {
        case O_INT:   *(int *) base = (int) strtol(v, &end, 10); break;
        case O_SIZE:  *(size_t *) base = (size_t) strtoull(v, &end, 10); break;
        case O_FLOAT: *(float *) base = strtof(v, &end); break;
        case O_STR:   snprintf(base, o->size, "%s", v); return 0;
        case O_BOOL:
            if (parse_bool(v, (int *) base)) break;
            return 0;
        case O_THINK:
            if (!strcasecmp(v, "default")) { *(int *) base = THIMBLE_THINK_DEFAULT; return 0; }
            { int b; if (!parse_bool(v, &b)) { *(int *) base = b ? THIMBLE_THINK_ON : THIMBLE_THINK_OFF; return 0; } }
            break;
        case O_LOG:
            if (!strcasecmp(v, "quiet")) { *(int *) base = 0; return 0; }
            if (!strcasecmp(v, "requests")) { *(int *) base = 1; return 0; }
            if (!strcasecmp(v, "verbose")) { *(int *) base = 2; return 0; }
            break;
    }
    if (end && *end == '\0' && end != v) return 0;
    snprintf(err, err_len, "invalid value '%s' for --%s", v, o->name);
    return -1;
}

static int add_model(server_config *c, const char *spec, char *err, size_t err_len) {
    if (c->n_models >= MAX_MODELS) { snprintf(err, err_len, "at most %d models", MAX_MODELS); return -1; }
    model_spec *m = &c->models[c->n_models++];
    const char *eq = strchr(spec, '=');
    if (eq) {
        snprintf(m->alias, sizeof(m->alias), "%s", eq + 1);
        m->path = strndup(spec, (size_t) (eq - spec));
    } else {
        m->path = strdup(spec);
    }
    return 0;
}

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) sz + 1);
    if (buf && fread(buf, 1, (size_t) sz, f) != (size_t) sz) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) buf[sz] = '\0', *n = (size_t) sz;
    return buf;
}

int server_config_load_file(server_config *c, const char *path, char *err, size_t err_len) {
    size_t n;
    char  *text = read_file(path, &n);
    if (!text) { snprintf(err, err_len, "cannot read config %s", path); return -1; }
    json *root = json_parse(text, n, err, err_len);
    free(text);
    if (!root || root->type != J_OBJ) {
        if (root) snprintf(err, err_len, "config %s must be a JSON object", path);
        json_free(root);
        return -1;
    }
    int rc = 0;
    for (const json *kv = root->child; kv && !rc; kv = kv->next) {
        if (!strcmp(kv->key, "models")) {   /* ["a.gguf", {"path": "b.gguf", "alias": "b"}] */
            for (const json *m = kv->type == J_ARR ? kv->child : NULL; m && !rc; m = m->next) {
                if (m->type == J_STR) rc = add_model(c, m->str, err, err_len);
                else if (m->type == J_OBJ && json_str(json_get(m, "path"), NULL)) {
                    rc = add_model(c, json_str(json_get(m, "path"), ""), err, err_len);
                    if (!rc && json_str(json_get(m, "alias"), NULL))
                        snprintf(c->models[c->n_models - 1].alias, sizeof(c->models[0].alias), "%s",
                                 json_str(json_get(m, "alias"), ""));
                }
            }
            continue;
        }
        if (!strcmp(kv->key, "system_prompt_file")) {
            size_t sn;
            char  *sp = read_file(json_str(kv, ""), &sn);
            if (!sp) { snprintf(err, err_len, "cannot read %s", json_str(kv, "")); rc = -1; break; }
            snprintf(c->system_prompt, sizeof(c->system_prompt), "%s", sp);
            free(sp);
            continue;
        }
        const option *o = find_option(kv->key);
        if (!o) { snprintf(err, err_len, "config %s: unknown key '%s'", path, kv->key); rc = -1; break; }
        char v[4096];
        if (kv->type == J_STR) snprintf(v, sizeof(v), "%s", kv->str);
        else if (kv->type == J_BOOL) snprintf(v, sizeof(v), "%s", kv->boolean ? "true" : "false");
        else if (kv->type == J_NUM) snprintf(v, sizeof(v), "%.17g", kv->num);
        else { snprintf(err, err_len, "config %s: bad value for '%s'", path, kv->key); rc = -1; break; }
        rc = set_option(c, o, v, err, err_len);
    }
    json_free(root);
    return rc;
}

int server_config_parse(server_config *c, int argc, char **argv, char *err, size_t err_len) {
    for (int i = 0; i < argc; i++)   /* config file first so flags override it */
        if (!strcmp(argv[i], "--config") && i + 1 < argc && server_config_load_file(c, argv[i + 1], err, err_len))
            return -1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2) != 0) {
            if (add_model(c, a, err, err_len)) return -1;
            continue;
        }
        const char *name = a + 2;
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(name, "config")) { i++; continue; }
        if (!strcmp(name, "model") || !strcmp(name, "alias") || !strcmp(name, "system-prompt-file")) {
            if (!next) { snprintf(err, err_len, "--%s needs a value", name); return -1; }
            i++;
            if (!strcmp(name, "model")) { if (add_model(c, next, err, err_len)) return -1; }
            else if (!strcmp(name, "alias")) {
                if (!c->n_models) { snprintf(err, err_len, "--alias must follow a model"); return -1; }
                snprintf(c->models[c->n_models - 1].alias, sizeof(c->models[0].alias), "%s", next);
            } else {
                size_t sn;
                char  *sp = read_file(next, &sn);
                if (!sp) { snprintf(err, err_len, "cannot read %s", next); return -1; }
                snprintf(c->system_prompt, sizeof(c->system_prompt), "%s", sp);
                free(sp);
            }
            continue;
        }
        const option *o = find_option(name);
        if (!o && !strncmp(name, "no-", 3) && (o = find_option(name + 3)) && o->type == O_BOOL) {
            *(int *) ((char *) c + o->offset) = 0;   /* --no-webui etc. */
            continue;
        }
        if (!o) { snprintf(err, err_len, "unknown option %s (see thimble serve --help)", a); return -1; }
        if (o->type == O_BOOL && (!next || !strncmp(next, "--", 2))) {   /* bare --webui */
            *(int *) ((char *) c + o->offset) = 1;
            continue;
        }
        if (!next) { snprintf(err, err_len, "%s needs a value", a); return -1; }
        if (set_option(c, o, next, err, err_len)) return -1;
        i++;
    }
    if (c->parallel < 1 || c->queue_size < 1) { snprintf(err, err_len, "--parallel and --queue must be >= 1"); return -1; }
    if (!c->n_models) { snprintf(err, err_len, "no model given"); return -1; }
    return 0;
}

void server_config_usage(void) {
    fprintf(stderr,
            "usage: thimble serve MODEL.gguf [MORE.gguf ...] [options]\n"
            "  --model PATH[=ALIAS]   add a model (clients pick it with \"model\")\n"
            "  --alias NAME           alias for the preceding model\n"
            "  --config FILE.json     settings file (same keys as below, '-' -> '_', plus \"models\")\n"
            "  --system-prompt-file F read the default system prompt from a file\n");
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++)
        fprintf(stderr, "  --%-20s %s\n", OPTIONS[i].name, OPTIONS[i].help);
    fprintf(stderr, "  boolean options accept on/off and --no-NAME\n");
}

char *server_config_json(const server_config *c) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{");
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(OPTIONS[0]); i++) {
        const option *o    = &OPTIONS[i];
        const char   *base = (const char *) c + o->offset;
        char key[64];
        snprintf(key, sizeof(key), "%s", o->name);
        for (char *k = key; *k; k++) if (*k == '-') *k = '_';
        sb_printf(&b, "%s\"%s\":", i ? "," : "", key);
        switch (o->type) {
            case O_INT: case O_LOG: sb_printf(&b, "%d", *(const int *) base); break;
            case O_SIZE:  sb_printf(&b, "%zu", *(const size_t *) base); break;
            case O_FLOAT: sb_printf(&b, "%g", (double) *(const float *) base); break;
            case O_BOOL:  sb_puts(&b, *(const int *) base ? "true" : "false"); break;
            case O_THINK: {
                int t = *(const int *) base;
                sb_puts(&b, t == THIMBLE_THINK_ON ? "\"on\"" : t == THIMBLE_THINK_OFF ? "\"off\"" : "\"default\"");
                break;
            }
            case O_STR:
                if (!strcmp(o->name, "api-key")) sb_puts(&b, base[0] ? "\"***\"" : "\"\"");
                else sb_json_str(&b, base, strlen(base));
                break;
        }
    }
    sb_puts(&b, "}");
    return b.s;
}
