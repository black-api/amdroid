#include "util.h"

#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char) s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static size_t pow2_at_least(size_t n) {
    size_t c = 16;
    while (c < n) c <<= 1;
    return c;
}

/* ---- strmap ---------------------------------------------------------- */

int strmap_init(strmap *m, size_t expected) {
    m->cap = pow2_at_least(expected * 2 + 1);
    m->n   = 0;
    m->e   = calloc(m->cap, sizeof(strmap_entry));
    return m->e ? 0 : -1;
}

void strmap_free(strmap *m) {
    if (!m->e) return;
    for (size_t i = 0; i < m->cap; i++) free(m->e[i].key);
    free(m->e);
    m->e = NULL;
}

int strmap_put(strmap *m, const char *key, size_t len, int val) {
    size_t i = fnv1a(key, len) & (m->cap - 1);
    while (m->e[i].key) {
        if (m->e[i].len == len && memcmp(m->e[i].key, key, len) == 0) {
            m->e[i].val = val;
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, key, len);
    copy[len]   = '\0';
    m->e[i].key = copy;
    m->e[i].len = len;
    m->e[i].val = val;
    m->n++;
    return 0;
}

int strmap_get(const strmap *m, const char *key, size_t len, int *out) {
    size_t i = fnv1a(key, len) & (m->cap - 1);
    while (m->e[i].key) {
        if (m->e[i].len == len && memcmp(m->e[i].key, key, len) == 0) {
            *out = m->e[i].val;
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return 0;
}

/* ---- pairmap --------------------------------------------------------- */

static uint64_t pair_key(int l, int r) {
    return ((uint64_t) (uint32_t) l << 32) | (uint32_t) r;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

int pairmap_init(pairmap *m, size_t expected) {
    m->cap = pow2_at_least(expected * 2 + 1);
    m->e   = calloc(m->cap, sizeof(pairmap_entry));
    return m->e ? 0 : -1;
}

void pairmap_free(pairmap *m) {
    free(m->e);
    m->e = NULL;
}

void pairmap_put(pairmap *m, int left, int right, int rank, int merged) {
    uint64_t k = pair_key(left, right);
    size_t   i = mix64(k) & (m->cap - 1);
    while (m->e[i].used && m->e[i].key != k) i = (i + 1) & (m->cap - 1);
    if (m->e[i].used) return;  /* keep the first (lowest) rank */
    m->e[i] = (pairmap_entry) { k, rank, merged, 1 };
}

int pairmap_get(const pairmap *m, int left, int right, int *rank, int *merged) {
    uint64_t k = pair_key(left, right);
    size_t   i = mix64(k) & (m->cap - 1);
    while (m->e[i].used) {
        if (m->e[i].key == k) {
            *rank   = m->e[i].rank;
            *merged = m->e[i].merged;
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return 0;
}

/* ---- utf8 ------------------------------------------------------------- */

int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

int utf8_decode(const char *s, size_t n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *) s;
    if (u[0] < 0x80 || n < 2) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t) (u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && n >= 3 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t) (u[0] & 0x0F) << 12) | ((uint32_t) (u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && n >= 4 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
        (u[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t) (u[0] & 0x07) << 18) | ((uint32_t) (u[1] & 0x3F) << 12) |
              ((uint32_t) (u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = u[0];  /* invalid sequence: treat the byte as its own code point */
    return 1;
}
