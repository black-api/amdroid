#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable int buffer -------------------------------------------- */

typedef struct {
    int *v;
    int  n, cap;
} ivec;

static void ivec_push(ivec *a, int x) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 64;
        a->v   = realloc(a->v, sizeof(int) * (size_t) a->cap);
    }
    a->v[a->n++] = x;
}

/* ---- loading --------------------------------------------------------- */

static void build_byte_maps(thimble_tokenizer *t) {
    /* GPT-2 bytes_to_unicode: printable bytes map to themselves, the rest to 256+n. */
    for (int i = 0; i < 512; i++) t->byte_dec[i] = -1;
    int n = 0;
    for (int b = 0; b < 256; b++) {
        int printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        uint32_t cp = printable ? (uint32_t) b : (uint32_t) (256 + n++);
        t->byte_enc_len[b] = utf8_encode(cp, t->byte_enc[b]);
        t->byte_dec[cp]    = b;
    }
}

static const thimble_tokenizer *g_sort_tok;   /* qsort has no context argument in C11 */

static int cmp_by_len_desc(const void *a, const void *b) {
    int ia = *(const int *) a, ib = *(const int *) b;
    return g_sort_tok->text_len[ib] - g_sort_tok->text_len[ia];
}

int tokenizer_find(const thimble_tokenizer *t, const char *token) {
    int id;
    return strmap_get(&t->vocab, token, strlen(token), &id) ? id : -1;
}

int tokenizer_load(thimble_tokenizer *t, const gguf_file *f, char *err, size_t err_len) {
    memset(t, 0, sizeof(*t));
    const gguf_kv *toks   = gguf_find_kv(f, "tokenizer.ggml.tokens");
    const gguf_kv *types  = gguf_find_kv(f, "tokenizer.ggml.token_type");
    const gguf_kv *merges = gguf_find_kv(f, "tokenizer.ggml.merges");
    gguf_str model;
    if (!gguf_get_str(f, "tokenizer.ggml.model", &model) || model.n != 4 ||
        memcmp(model.s, "gpt2", 4) != 0) {
        snprintf(err, err_len, "tokenizer: expected tokenizer.ggml.model = gpt2");
        return -1;
    }
    if (!toks || toks->type != GGUF_ARRAY || toks->arr_type != GGUF_STRING) {
        snprintf(err, err_len, "tokenizer: missing tokenizer.ggml.tokens");
        return -1;
    }
    t->n_vocab  = (int) toks->arr_n;
    t->text     = calloc((size_t) t->n_vocab, sizeof(char *));
    t->text_len = calloc((size_t) t->n_vocab, sizeof(int));
    t->type     = calloc((size_t) t->n_vocab, sizeof(int));
    t->special  = calloc((size_t) t->n_vocab, sizeof(int));
    if (!t->text || !t->text_len || !t->type || !t->special ||
        strmap_init(&t->vocab, (size_t) t->n_vocab) != 0) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }

    const uint8_t *cur = NULL;
    for (int i = 0; i < t->n_vocab; i++) {
        gguf_str s     = gguf_arr_next_str(toks, &cur);
        t->text[i]     = malloc(s.n + 1);
        memcpy(t->text[i], s.s, s.n);
        t->text[i][s.n] = '\0';
        t->text_len[i] = (int) s.n;
        t->type[i]     = types ? (int) gguf_arr_int(types, (uint64_t) i) : TOK_NORMAL;
        if (t->type[i] != TOK_UNUSED) strmap_put(&t->vocab, s.s, s.n, i);
        if (t->type[i] == TOK_CONTROL && s.n > 0) t->special[t->n_special++] = i;
    }
    g_sort_tok = t;
    qsort(t->special, (size_t) t->n_special, sizeof(int), cmp_by_len_desc);

    size_t n_merges = merges ? (size_t) merges->arr_n : 0;
    pairmap_init(&t->merges, n_merges + 1);
    cur = NULL;
    char buf[1024];
    for (size_t r = 0; r < n_merges; r++) {
        gguf_str s = gguf_arr_next_str(merges, &cur);
        const char *sp = memchr(s.s, ' ', s.n);
        if (!sp || s.n >= sizeof(buf)) continue;
        size_t nl = (size_t) (sp - s.s), nr = s.n - nl - 1;
        int l, rr, m;
        memcpy(buf, s.s, nl);
        memcpy(buf + nl, sp + 1, nr);
        if (strmap_get(&t->vocab, s.s, nl, &l) && strmap_get(&t->vocab, sp + 1, nr, &rr) &&
            strmap_get(&t->vocab, buf, nl + nr, &m)) {
            pairmap_put(&t->merges, l, rr, (int) r, m);
        }
    }

    build_byte_maps(t);
    int64_t v;
    t->bos = gguf_get_int(f, "tokenizer.ggml.bos_token_id", &v) ? (int) v : tokenizer_find(t, "<|bos|>");
    t->eos = gguf_get_int(f, "tokenizer.ggml.eos_token_id", &v) ? (int) v : tokenizer_find(t, "<|eos|>");
    t->pad = gguf_get_int(f, "tokenizer.ggml.padding_token_id", &v) ? (int) v : tokenizer_find(t, "<|pad|>");
    t->unk = gguf_get_int(f, "tokenizer.ggml.unknown_token_id", &v) ? (int) v : tokenizer_find(t, "<|unk|>");
    t->think     = tokenizer_find(t, "<|think|>");
    t->think_end = tokenizer_find(t, "<|/think|>");
    return 0;
}

void tokenizer_free(thimble_tokenizer *t) {
    if (t->text)
        for (int i = 0; i < t->n_vocab; i++) free(t->text[i]);
    free(t->text);
    free(t->text_len);
    free(t->type);
    free(t->special);
    strmap_free(&t->vocab);
    pairmap_free(&t->merges);
    memset(t, 0, sizeof(*t));
}

/* ---- GPT-2 pre-tokenizer --------------------------------------------- */
/* Regex: 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
 * Unicode classes are approximated: ASCII exactly; non-ASCII code points count as
 * letters except whitespace and the Latin-1 / general punctuation blocks. */

enum { CL_SPACE, CL_LETTER, CL_DIGIT, CL_OTHER };

static int char_class(uint32_t c) {
    if (c == ' ' || (c >= '\t' && c <= '\r') || c == 0x85 || c == 0xA0 || c == 0x1680 ||
        (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
        c == 0x3000)
        return CL_SPACE;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return CL_LETTER;
    if (c >= '0' && c <= '9') return CL_DIGIT;
    if (c < 0x80) return CL_OTHER;
    if ((c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x206F) ||
        (c >= 0x3000 && c <= 0x303F))
        return CL_OTHER;
    return CL_LETTER;
}

static int class_at(const char *s, size_t n, size_t i, size_t *len) {
    uint32_t cp;
    *len = (size_t) utf8_decode(s + i, n - i, &cp);
    return char_class(cp);
}

/* Returns the end of the pre-token starting at i. */
static size_t pretok_next(const char *s, size_t n, size_t i) {
    static const char *contractions[] = { "s", "t", "re", "ve", "m", "ll", "d" };
    if (s[i] == '\'') {
        for (int k = 0; k < 7; k++) {
            size_t cl = strlen(contractions[k]);
            if (i + 1 + cl <= n && memcmp(s + i + 1, contractions[k], cl) == 0) return i + 1 + cl;
        }
    }
    size_t len, j = i;
    if (s[i] == ' ' && i + 1 < n) j = i + 1;
    int cls = class_at(s, n, j, &len);
    if (cls != CL_SPACE) {   /* ' ?\p{L}+' | ' ?\p{N}+' | ' ?[^\s\p{L}\p{N}]+' */
        size_t e = j;
        while (e < n) {
            size_t l;
            if (class_at(s, n, e, &l) != cls) break;
            e += l;
        }
        return e;
    }
    /* whitespace run starting at i */
    size_t e = i, last = i;
    while (e < n) {
        size_t l;
        if (class_at(s, n, e, &l) != CL_SPACE) break;
        last = e;
        e += l;
    }
    if (e == n) return e;          /* \s+ to the end */
    if (last > i) return last;     /* \s+(?!\S): leave the last space for the next word */
    return e;                      /* single whitespace char */
}

/* ---- BPE ----------------------------------------------------------------- */

static void bpe_word(const thimble_tokenizer *t, const char *s, size_t n, ivec *out) {
    /* byte-level map, then one symbol per mapped character */
    int *sym = malloc(sizeof(int) * (n + 1));
    int  ns  = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char) s[i];
        int id;
        sym[ns++] = strmap_get(&t->vocab, t->byte_enc[b], (size_t) t->byte_enc_len[b], &id) ? id : t->unk;
    }
    for (;;) {
        int best = -1, best_rank = 0, best_merged = 0;
        for (int i = 0; i + 1 < ns; i++) {
            int rank, merged;
            if (sym[i] < 0 || sym[i + 1] < 0) continue;
            if (pairmap_get(&t->merges, sym[i], sym[i + 1], &rank, &merged) &&
                (best < 0 || rank < best_rank)) {
                best = i, best_rank = rank, best_merged = merged;
            }
        }
        if (best < 0) break;
        sym[best] = best_merged;
        memmove(sym + best + 1, sym + best + 2, sizeof(int) * (size_t) (ns - best - 2));
        ns--;
    }
    for (int i = 0; i < ns; i++) ivec_push(out, sym[i]);
    free(sym);
}

static void encode_plain(const thimble_tokenizer *t, const char *s, size_t n, ivec *out) {
    size_t i = 0;
    while (i < n) {
        size_t e = pretok_next(s, n, i);
        if (e <= i) e = i + 1;
        bpe_word(t, s + i, e - i, out);
        i = e;
    }
}

int tokenizer_encode(const thimble_tokenizer *t, const char *text, int *out, int max_out) {
    ivec   ids = { 0 };
    size_t n = strlen(text), start = 0, i = 0;
    while (i < n) {
        int hit = -1;
        for (int k = 0; k < t->n_special; k++) {
            int id = t->special[k];
            size_t l = (size_t) t->text_len[id];
            if (l <= n - i && memcmp(text + i, t->text[id], l) == 0) { hit = id; break; }
        }
        if (hit >= 0) {
            encode_plain(t, text + start, i - start, &ids);
            ivec_push(&ids, hit);
            i += (size_t) t->text_len[hit];
            start = i;
        } else {
            i++;
        }
    }
    encode_plain(t, text + start, n - start, &ids);
    for (int k = 0; k < ids.n && k < max_out; k++) out[k] = ids.v[k];
    int total = ids.n;
    free(ids.v);
    return total;
}

int tokenizer_piece(const thimble_tokenizer *t, int id, char *buf, int len, int with_special) {
    if (id < 0 || id >= t->n_vocab) return 0;
    if (t->type[id] == TOK_CONTROL || t->type[id] == TOK_UNUSED) {
        if (!with_special) return 0;
        int l = t->text_len[id];
        memcpy(buf, t->text[id], (size_t) (l < len ? l : len));
        return l;
    }
    const char *s = t->text[id];
    size_t      n = (size_t) t->text_len[id], i = 0;
    int         w = 0;
    while (i < n) {
        uint32_t cp;
        int      l = utf8_decode(s + i, n - i, &cp);
        int      b = cp < 512 ? t->byte_dec[cp] : -1;
        if (b >= 0) {
            if (w < len) buf[w] = (char) b;
            w++;
        } else {   /* not a byte-level char: copy the UTF-8 as is */
            for (int k = 0; k < l; k++, w++)
                if (w < len) buf[w] = s[i + k];
        }
        i += (size_t) l;
    }
    return w;
}
