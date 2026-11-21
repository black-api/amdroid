/* Small containers: string -> int map and (int, int) -> (int, int) map. */
#ifndef THIMBLE_UTIL_H
#define THIMBLE_UTIL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *key;
    size_t len;
    int    val;
} strmap_entry;

typedef struct {
    strmap_entry *e;
    size_t        cap;
    size_t        n;
} strmap;

int  strmap_init(strmap *m, size_t expected);
void strmap_free(strmap *m);
int  strmap_put(strmap *m, const char *key, size_t len, int val);          /* 0 ok, -1 oom */
int  strmap_get(const strmap *m, const char *key, size_t len, int *out);   /* 1 found */

/* Pair map used for BPE merges: (left, right) -> (rank, merged id). */
typedef struct {
    uint64_t key;
    int      rank;
    int      merged;
    int      used;
} pairmap_entry;

typedef struct {
    pairmap_entry *e;
    size_t         cap;
} pairmap;

int  pairmap_init(pairmap *m, size_t expected);
void pairmap_free(pairmap *m);
void pairmap_put(pairmap *m, int left, int right, int rank, int merged);
int  pairmap_get(const pairmap *m, int left, int right, int *rank, int *merged);

/* UTF-8 helpers. */
int utf8_encode(uint32_t cp, char *out);                         /* returns bytes (1-4) */
int utf8_decode(const char *s, size_t n, uint32_t *cp);          /* returns bytes consumed (>=1) */

#endif
