/* thimble - command line front end for the Thimble engine.
 *
 *   thimble info     MODEL.gguf
 *   thimble tokenize MODEL.gguf TEXT
 *   thimble logits   MODEL.gguf ID [ID ...]        one line of logits per position
 *   thimble run      MODEL.gguf [options] PROMPT    raw text completion
 *   thimble run      MODEL.gguf --chat [options] MESSAGE
 *   thimble chat     MODEL.gguf [options]           interactive chat
 *   thimble serve    MODEL.gguf [MORE.gguf] [options]   OpenAI-compatible HTTP API (serve --help)
 *   thimble speak    "text" | -  [--voice NAME|PATH] [--out FILE.wav] [--threads N]
 *                                                   text to speech ("-" reads stdin)
 *   thimble voice    [NAME]                          show the installed voice
 *
 * options: -n N (max new tokens, default 128)  --temp T  --top-k K  --top-p P
 *          --min-p P  --repeat-penalty R  --seed S  --think | --no-think
 *          --show-think (print reasoning)  --ctx N
 */
#include "thimble.h"
#include "server.h"

int speak_main(int argc, char **argv);
int voice_main(int argc, char **argv);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    thimble_sampling sampling;
    int              max_new;
    int              thinking;
    int              chat;
    int              show_think;
    int              n_ctx;
} options;

static void usage(void) {
    fprintf(stderr,
            "usage: thimble info|tokenize|logits|run|chat|serve MODEL.gguf [options] [args]\n"
            "       thimble speak \"text\"|- [--voice NAME|PATH] [--out FILE.wav] [--threads N]\n"
            "  serve: see 'thimble serve --help'\n"
            "  run options: -n N  --temp T  --top-k K  --top-p P  --min-p P  --repeat-penalty R\n"
            "               --seed S  --chat  --think|--no-think  --show-think  --ctx N\n");
}

/* Parse options; non-option arguments are collected into rest. */
static int parse_opts(int argc, char **argv, options *o, char **rest, int *n_rest) {
    o->sampling   = thimble_sampling_default();
    o->max_new    = 128;
    o->thinking   = THIMBLE_THINK_DEFAULT;
    o->chat       = 0;
    o->show_think = 0;
    o->n_ctx      = 0;
    *n_rest       = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s needs a value\n", a), exit(2), ""))
        if      (!strcmp(a, "-n"))               o->max_new = atoi(NEXT());
        else if (!strcmp(a, "--temp"))           o->sampling.temperature = (float) atof(NEXT());
        else if (!strcmp(a, "--top-k"))          o->sampling.top_k = atoi(NEXT());
        else if (!strcmp(a, "--top-p"))          o->sampling.top_p = (float) atof(NEXT());
        else if (!strcmp(a, "--min-p"))          o->sampling.min_p = (float) atof(NEXT());
        else if (!strcmp(a, "--repeat-penalty")) o->sampling.repetition_penalty = (float) atof(NEXT());
        else if (!strcmp(a, "--seed"))           o->sampling.seed = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--ctx"))            o->n_ctx = atoi(NEXT());
        else if (!strcmp(a, "--chat"))           o->chat = 1;
        else if (!strcmp(a, "--think"))          o->thinking = THIMBLE_THINK_ON;
        else if (!strcmp(a, "--no-think"))       o->thinking = THIMBLE_THINK_OFF;
        else if (!strcmp(a, "--show-think"))     o->show_think = 1;
        else if (a[0] == '-' && a[1] == '-')     { fprintf(stderr, "unknown option %s\n", a); return -1; }
        else                                     rest[(*n_rest)++] = argv[i];
#undef NEXT
    }
    if (o->sampling.temperature > 0 && !o->sampling.seed) o->sampling.seed = (uint64_t) time(NULL);
    return 0;
}

static int *tokenize_alloc(const thimble_model *m, const char *text, int *n) {
    *n = thimble_tokenize(m, text, NULL, 0);
    int *ids = malloc(sizeof(int) * (size_t) (*n + 1));
    if (ids) thimble_tokenize(m, text, ids, *n);
    return ids;
}

/* ---- streaming output ------------------------------------------------- */

typedef struct {
    int   show_think;
    int   in_think;
    char *answer;      /* accumulated answer (for chat history) */
    size_t len, cap;
} stream_state;

static int on_token(int token, const char *piece, int len, int is_reasoning, void *user) {
    (void) token;
    stream_state *st = user;
    if (is_reasoning) {
        if (st->show_think) {
            if (!st->in_think) fputs("\x1b[2m[think] ", stdout), st->in_think = 1;
            fwrite(piece, 1, (size_t) len, stdout);
        }
    } else {
        if (st->in_think) fputs("\x1b[0m\n", stdout), st->in_think = 0;
        fwrite(piece, 1, (size_t) len, stdout);
        if (st->len + (size_t) len + 1 > st->cap) {
            st->cap    = (st->cap + (size_t) len + 1) * 2;
            st->answer = realloc(st->answer, st->cap);
        }
        memcpy(st->answer + st->len, piece, (size_t) len);
        st->len += (size_t) len;
        st->answer[st->len] = '\0';
    }
    fflush(stdout);
    return 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n')) s[--n] = '\0';
    return s;
}

/* ---- commands --------------------------------------------------------- */

static int cmd_logits(thimble_model *m, int argc, char **argv) {
    thimble_ctx *c = thimble_ctx_new(m, argc + 1);
    if (!c) return 1;
    int nv = thimble_n_vocab(m);
    for (int i = 0; i < argc; i++) {
        const float *lg = thimble_eval(c, atoi(argv[i]));
        if (!lg) { fprintf(stderr, "eval failed at %d\n", i); return 1; }
        for (int j = 0; j < nv; j++) printf(j ? " %.7g" : "%.7g", lg[j]);
        printf("\n");
    }
    thimble_ctx_free(c);
    return 0;
}

static int cmd_tokenize(thimble_model *m, const char *text) {
    int  n;
    int *ids = tokenize_alloc(m, text, &n);
    for (int i = 0; i < n; i++) printf(i ? " %d" : "%d", ids[i]);
    printf("\n");
    free(ids);
    return 0;
}

static int run_prompt(thimble_model *m, thimble_ctx *c, const char *prompt, const options *o,
                      stream_state *st) {
    int  n;
    int *ids = tokenize_alloc(m, prompt, &n);
    if (!ids || n == 0) { fprintf(stderr, "empty prompt\n"); free(ids); return 1; }
    thimble_gen_result r;
    int rc = thimble_generate(c, ids, n, &o->sampling, o->max_new, on_token, st, &r);
    free(ids);
    if (st->in_think) fputs("\x1b[0m", stdout), st->in_think = 0;
    printf("\n");
    if (rc < 0) { fprintf(stderr, "generation failed (prompt longer than the context?)\n"); return 1; }
    return 0;
}

static int cmd_run(thimble_model *m, const options *o, const char *text) {
    thimble_ctx *c = thimble_ctx_new(m, o->n_ctx);
    if (!c) return 1;
    char *prompt;
    if (o->chat) {
        thimble_message msg = { "user", text };
        prompt = thimble_render_chat(m, &msg, 1, o->thinking);
        if (!prompt) { fprintf(stderr, "this model was not trained with think mode\n"); return 1; }
    } else {
        prompt = strdup(text);
    }
    stream_state st = { o->show_think, 0, NULL, 0, 0 };
    int rc = run_prompt(m, c, prompt, o, &st);
    free(st.answer);
    free(prompt);
    thimble_ctx_free(c);
    return rc;
}

static int cmd_chat(thimble_model *m, const options *o) {
    thimble_ctx     *c = thimble_ctx_new(m, o->n_ctx);
    thimble_message *hist = NULL;
    int              n_hist = 0;
    char             line[4096];
    if (!c) return 1;
    printf("Thimble chat (%s%s). Type 'exit' to quit.\n", thimble_arch(m),
           thimble_has_thinking(m) ? ", think mode" : "");
    for (;;) {
        printf("you> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char *msg = trim(line);
        if (!strcmp(msg, "exit") || !strcmp(msg, "quit")) break;
        if (!*msg) continue;
        hist = realloc(hist, sizeof(thimble_message) * (size_t) (n_hist + 2));
        hist[n_hist++] = (thimble_message) { "user", strdup(msg) };
        char *prompt = thimble_render_chat(m, hist, n_hist, o->thinking);
        if (!prompt) { fprintf(stderr, "this model was not trained with think mode\n"); break; }
        thimble_ctx_reset(c);
        printf("bot> ");
        stream_state st = { o->show_think, 0, NULL, 0, 0 };
        run_prompt(m, c, prompt, o, &st);
        free(prompt);
        hist[n_hist++] = (thimble_message) { "assistant", strdup(st.answer ? trim(st.answer) : "") };
        free(st.answer);
    }
    for (int i = 0; i < n_hist; i++) free((char *) hist[i].content);
    free(hist);
    thimble_ctx_free(c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "serve")) return server_main(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "speak")) return speak_main(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "voice")) return voice_main(argc - 2, argv + 2);
    if (argc < 3) { usage(); return 2; }
    const char *cmd = argv[1];
    char err[512];
    thimble_model *m = thimble_model_load(argv[2], err, sizeof(err));
    if (!m) { fprintf(stderr, "error: %s\n", err); return 1; }

    char   **rest = calloc((size_t) argc, sizeof(char *));
    int      n_rest = 0, rc = 2;
    options  o;
    if (!strcmp(cmd, "logits")) {
        rc = cmd_logits(m, argc - 3, argv + 3);
    } else if (parse_opts(argc - 3, argv + 3, &o, rest, &n_rest) != 0) {
        rc = 2;
    } else if (!strcmp(cmd, "info")) {
        char buf[4096];
        thimble_describe(m, buf, sizeof(buf));
        printf("%s\n", buf);
        rc = 0;
    } else if (!strcmp(cmd, "tokenize") && n_rest == 1) {
        rc = cmd_tokenize(m, rest[0]);
    } else if (!strcmp(cmd, "run") && n_rest == 1) {
        rc = cmd_run(m, &o, rest[0]);
    } else if (!strcmp(cmd, "chat")) {
        rc = cmd_chat(m, &o);

    } else {
        usage();
    }
    free(rest);
    thimble_model_free(m);
    return rc;
}
