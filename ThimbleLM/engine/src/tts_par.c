/* A tiny fork/join pool for voice synthesis.
 *
 * Every parallel region splits a loop whose iterations are independent and
 * write to disjoint memory, so results do not depend on the thread count:
 * the output is bit-identical to running single-threaded.
 *
 * Thread count: $THIMBLE_VOICE_THREADS, else the number of online cores capped
 * at 8. 1 runs everything on the calling thread and starts no threads at all.
 */
#include "tts.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    pthread_t      id;
    struct pool   *pool;
    int            index;
} worker;

struct pool {
    worker         *w;
    int             n;              /* workers, i.e. threads - 1 */
    pthread_mutex_t mu;
    pthread_cond_t  go, done;
    tts_par_fn      fn;
    void           *ctx;
    int             total, chunks, next, finished, generation, stop;
};

static struct pool *POOL;
static pthread_once_t POOL_ONCE = PTHREAD_ONCE_INIT;
static int THREADS = -1;

int tts_threads(void) {
    if (THREADS < 0) {
        const char *e = getenv("THIMBLE_VOICE_THREADS");
        long n = e && *e ? strtol(e, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > 8) n = 8;
        THREADS = (int)n;
    }
    return THREADS;
}

void tts_set_threads(int n) { THREADS = n < 1 ? 1 : n > 8 ? 8 : n; }

/* Claim chunks until the region is done; shared by the workers and the caller. */
static void run_chunks(struct pool *p) {
    for (;;) {
        pthread_mutex_lock(&p->mu);
        int i = p->next < p->chunks ? p->next++ : -1;
        pthread_mutex_unlock(&p->mu);
        if (i < 0) return;
        int begin = (int)((long)p->total * i / p->chunks);
        int end = (int)((long)p->total * (i + 1) / p->chunks);
        if (end > begin) p->fn(p->ctx, begin, end);
    }
}

static void *worker_main(void *arg) {
    worker *w = arg;
    struct pool *p = w->pool;
    int seen = 0;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->stop && p->generation == seen) pthread_cond_wait(&p->go, &p->mu);
        if (p->stop) { pthread_mutex_unlock(&p->mu); return NULL; }
        seen = p->generation;
        pthread_mutex_unlock(&p->mu);
        run_chunks(p);
        pthread_mutex_lock(&p->mu);
        if (++p->finished == p->n) pthread_cond_signal(&p->done);
        pthread_mutex_unlock(&p->mu);
    }
}

static void pool_init(void) {
    int threads = tts_threads();
    if (threads < 2) return;
    struct pool *p = calloc(1, sizeof *p);
    if (!p) return;
    p->n = threads - 1;
    p->w = calloc((size_t)p->n, sizeof(worker));
    if (!p->w) { free(p); return; }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->go, NULL);
    pthread_cond_init(&p->done, NULL);
    for (int i = 0; i < p->n; i++) {
        p->w[i].pool = p; p->w[i].index = i;
        if (pthread_create(&p->w[i].id, NULL, worker_main, &p->w[i])) { p->n = i; break; }
    }
    if (p->n < 1) { free(p->w); free(p); return; }
    POOL = p;
}

void tts_parallel_for(int n, tts_par_fn fn, void *ctx) {
    if (n <= 0) return;
    if (tts_threads() > 1) pthread_once(&POOL_ONCE, pool_init);
    struct pool *p = POOL;
    if (!p || n == 1) { fn(ctx, 0, n); return; }
    /* a few chunks per thread, so uneven iterations still balance */
    int chunks = (p->n + 1) * 4;
    if (chunks > n) chunks = n;
    pthread_mutex_lock(&p->mu);
    p->fn = fn; p->ctx = ctx; p->total = n; p->chunks = chunks; p->next = 0; p->finished = 0;
    p->generation++;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->mu);
    run_chunks(p);                       /* the caller is a worker too */
    pthread_mutex_lock(&p->mu);
    while (p->finished < p->n) pthread_cond_wait(&p->done, &p->mu);
    pthread_mutex_unlock(&p->mu);
}
