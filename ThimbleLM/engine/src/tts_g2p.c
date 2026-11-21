/* English text -> phoneme ids, in C.
 *
 * A port of thimble/tts/g2p/english.py (dictionary-first, rule-based stress and
 * suffixes, number spelling, neural fallback for unknown words), so that the
 * engine can speak with no Python. Names follow the Python file so the two can
 * be read side by side. All text is handled as one byte per symbol: ASCII as
 * itself and every other symbol the front end cares about as a code from 0x80
 * (see TTS_SYMBOLS_UTF8 in tts.h); accented Latin letters are folded to ASCII
 * on the way in. Strings live in a per-call arena and are never freed one by one.
 */
#include "tts.h"
#include "tts_tables.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== arena + strings */
typedef struct blk { struct blk *next; size_t used, cap; unsigned char d[]; } blk;
typedef struct { blk *head; } arena;

static void *aalloc(arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > 65536 ? n : 65536;
        blk *b = malloc(sizeof(blk) + cap);
        if (!b) abort();
        b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
    }
    void *p = a->head->d + a->head->used;
    a->head->used += n;
    return p;
}
static void afree(arena *a) { for (blk *b = a->head, *n; b; b = n) { n = b->next; free(b); } a->head = NULL; }

typedef struct { const unsigned char *p; int n; } S;           /* n < 0: Python None */
#define NONE_S ((S){NULL, -1})
#define EMPTY_S ((S){(const unsigned char *)"", 0})
static int is_none(S s) { return s.n < 0; }
static S mk(const char *lit) { return (S){(const unsigned char *)lit, (int)strlen(lit)}; }
static S adup(arena *a, const unsigned char *p, int n) {
    unsigned char *d = aalloc(a, (size_t)n + 1); memcpy(d, p, (size_t)n); d[n] = 0; return (S){d, n};
}
static S scat(arena *a, S x, S y) {
    unsigned char *d = aalloc(a, (size_t)x.n + (size_t)y.n + 1);
    memcpy(d, x.p, (size_t)x.n); memcpy(d + x.n, y.p, (size_t)y.n); d[x.n + y.n] = 0; return (S){d, x.n + y.n};
}
static S sub(S s, int from, int to) { if (from < 0) from = 0; if (to > s.n) to = s.n; if (to < from) to = from; return (S){s.p + from, to - from}; }
static int seq(S x, S y) { return x.n == y.n && (x.n <= 0 || !memcmp(x.p, y.p, (size_t)x.n)); }
static int seql(S x, const char *lit) { return seq(x, mk(lit)); }
static int starts(S x, const char *lit) { int n = (int)strlen(lit); return x.n >= n && !memcmp(x.p, lit, (size_t)n); }
static int ends(S x, const char *lit) { int n = (int)strlen(lit); return x.n >= n && !memcmp(x.p + x.n - n, lit, (size_t)n); }
static int has(S x, int c) { for (int i = 0; i < x.n; i++) if (x.p[i] == (unsigned char)c) return 1; return 0; }
static int count_c(S x, int c) { int k = 0; for (int i = 0; i < x.n; i++) k += x.p[i] == (unsigned char)c; return k; }

/* Accented Latin letters get their own codes (0xB0..0xE3: capitals, then small letters, in
 * A-Z order) so they count as letters (Python's isalpha) yet never match a dictionary key,
 * exactly as non-ASCII words behave in the Python front end. The fallback folds them back. */
#define ACC_UP 0xB0
#define ACC_LO 0xCA
static int is_acc(int c) { return c >= ACC_UP && c < ACC_LO + 26; }
static int acc_base(int c) { return c < ACC_LO ? 'A' + (c - ACC_UP) : 'a' + (c - ACC_LO); }
static int isup_a(int c) { return c >= 'A' && c <= 'Z'; }
static int islo_a(int c) { return c >= 'a' && c <= 'z'; }
static int isal_a(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int isal(int c) { return isal_a(c) || is_acc(c); }
static int isdig(int c) { return c >= '0' && c <= '9'; }
static int isup(int c) { return isup_a(c) || (c >= ACC_UP && c < ACC_LO); }
static int islo(int c) { return islo_a(c) || (c >= ACC_LO && c < ACC_LO + 26); }
static int lowc(int c) { return isup_a(c) ? c + 32 : (c >= ACC_UP && c < ACC_LO) ? c + 26 : c; }
static int upc(int c) { return islo_a(c) ? c - 32 : (c >= ACC_LO && c < ACC_LO + 26) ? c - 26 : c; }
static S slower(arena *a, S s) { S o = adup(a, s.p, s.n); for (int i = 0; i < o.n; i++) ((unsigned char *)o.p)[i] = (unsigned char)lowc(o.p[i]); return o; }
static S supper(arena *a, S s) { S o = adup(a, s.p, s.n); for (int i = 0; i < o.n; i++) ((unsigned char *)o.p)[i] = (unsigned char)upc(o.p[i]); return o; }
static int s_isalpha(S s) { if (s.n <= 0) return 0; for (int i = 0; i < s.n; i++) if (!isal(s.p[i])) return 0; return 1; }
static int all_digits(S s) { if (s.n <= 0) return 0; for (int i = 0; i < s.n; i++) if (!isdig(s.p[i])) return 0; return 1; }
static S replace_c(arena *a, S s, int from, const char *to) {
    int tl = (int)strlen(to), n = 0;
    for (int i = 0; i < s.n; i++) n += s.p[i] == (unsigned char)from ? tl : 1;
    unsigned char *d = aalloc(a, (size_t)n + 1), *o = d;
    for (int i = 0; i < s.n; i++) { if (s.p[i] == (unsigned char)from) { memcpy(o, to, (size_t)tl); o += tl; } else *o++ = s.p[i]; }
    *o = 0; return (S){d, n};
}
static S strip_set(S s, const char *set) {
    int i = 0, j = s.n;
    while (i < j && s.p[i] && strchr(set, s.p[i])) i++;
    while (j > i && s.p[j - 1] && strchr(set, s.p[j - 1])) j--;
    return sub(s, i, j);
}

/* ================================================================== symbol sets */
#define P_PRIMARY S_PRIMARY
#define P_SECONDARY S_SECONDARY
static int is_vowel(int c) {
    switch (c) { case 'A': case 'I': case 'O': case 'Q': case 'W': case 'Y': case 'a': case 'i': case 'u':
    case S_AE: case S_OPENA: case S_OPENO: case S_SCHWA: case S_OPENE: case S_REVE: case S_IH: case S_UH: case S_WEDGE: case S_IBAR: return 1; }
    return 0;
}
static int is_consonant(int c) {
    switch (c) { case 'b': case 'd': case 'f': case 'h': case 'j': case 'k': case 'l': case 'm': case 'n': case 'p':
    case 's': case 't': case 'v': case 'w': case 'z': case S_ETH: case S_ENG: case S_G: case S_R: case S_TAP:
    case S_SH: case S_ZH: case S_DJ: case S_TCH: case S_THETA: return 1; }
    return 0;
}
static int is_us_tau(int c) {
    switch (c) { case 'A': case 'I': case 'O': case 'W': case 'Y': case 'i': case 'u': case S_AE: case S_OPENA:
    case S_SCHWA: case S_OPENE: case S_IH: case S_R: case S_UH: case S_WEDGE: return 1; }
    return 0;
}
static int is_diphthong(int c) { switch (c) { case 'A': case 'I': case 'O': case 'Q': case 'W': case 'Y': case S_DJ: case S_TCH: return 1; } return 0; }
static int is_punct(int c) {
    switch (c) { case ';': case ':': case ',': case '.': case '!': case '?': case S_EMDASH: case S_ELLIPSIS: case '"': case S_LDQUOTE: case S_RDQUOTE: return 1; }
    return 0;
}
static int is_nonquote_punct(int c) { return is_punct(c) && c != '"' && c != S_LDQUOTE && c != S_RDQUOTE; }
static int is_subtoken_junk(int c) { return c == '\'' || c == ',' || c == '-' || c == '.' || c == '_' || c == S_LSQUOTE || c == S_RSQUOTE || c == '/'; }
static int is_quote_ch(int c) { return c == '\'' || c == S_LSQUOTE || c == S_RSQUOTE; }
static int is_stress(int c) { return c == P_PRIMARY || c == P_SECONDARY; }
static int stress_weight(S ps) { int w = 0; for (int i = 0; i < ps.n; i++) w += is_diphthong(ps.p[i]) ? 2 : 1; return w; }

/* ================================================================== tokens */
typedef struct Tok {
    S text; char tag[8]; S ws; S ph;           /* ph.n < 0: None */
    int has_alias; S alias;
    int has_stress; double stress;
    unsigned char currency;                    /* 0: None */
    char flags[8];
    int prespace, rating, is_head;             /* rating -1: None */
} Tok;

static void set_tag(Tok *t, const char *tag) { snprintf(t->tag, sizeof t->tag, "%s", tag); }
static int tag_is(const Tok *t, const char *tag) { return !strcmp(t->tag, tag); }

typedef struct { int future_vowel; int future_to; } TokCtx;   /* future_vowel: -1 None, 0 False, 1 True */

typedef struct {
    const thimble_voice *v;
    arena *a;
    char err[160];
    int fail;                    /* -1 other, -2 empty, -3 too long (the first error wins) */
} G;

static void fail(G *g, int kind, const char *msg) { if (!g->fail) { g->fail = kind; snprintf(g->err, sizeof g->err, "%s", msg); } }

/* ================================================================== dictionary */
typedef struct { int kind; S s; const unsigned char *d; } Ent;   /* 0 missing, 1 string, 2 tag dict, 3 None */

static Ent dict_get(const tts_dict *d, S key) {
    int lo = 0, hi = d->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const unsigned char *rec = d->blob + d->index[mid];
        size_t kl = strlen((const char *)rec);
        size_t m = kl < (size_t)key.n ? kl : (size_t)key.n;
        int c = memcmp(rec, key.p, m);
        if (!c) c = kl < (size_t)key.n ? -1 : kl > (size_t)key.n ? 1 : 0;
        if (c == 0) {
            const unsigned char *val = rec + kl + 1;
            Ent e = {0, EMPTY_S, NULL};
            if (val[0] == 0x01) { e.kind = 2; e.d = val + 1; }
            else if (val[0] == 0x03 && val[1] == 0) e.kind = 3;
            else { e.kind = 1; e.s = (S){val, (int)strlen((const char *)val)}; }
            return e;
        }
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return (Ent){0, EMPTY_S, NULL};
}

/* a tag dict is "TAG\x02value" items separated by \x01 */
static int ent_dict_find(const Ent *e, const char *tag, Ent *out) {
    if (e->kind != 2 || !tag) return 0;
    size_t tl = strlen(tag);
    const unsigned char *p = e->d;
    for (;;) {
        const unsigned char *sep = (const unsigned char *)memchr(p, 0x02, strlen((const char *)p));
        if (!sep) return 0;
        const unsigned char *vs = sep + 1, *end = vs;
        while (*end && *end != 0x01) end++;
        if ((size_t)(sep - p) == tl && !memcmp(p, tag, tl)) {
            if (end - vs == 1 && vs[0] == 0x03) { out->kind = 3; out->s = EMPTY_S; }
            else { out->kind = 1; out->s = (S){vs, (int)(end - vs)}; }
            return 1;
        }
        if (!*end) return 0;
        p = end + 1;
    }
}

typedef struct { G *g; const tts_dict *gold, *silver; } Lx;

static Ent gold_get(Lx *x, S w) { return dict_get(x->gold, w); }
static Ent silver_get(Lx *x, S w) { return dict_get(x->silver, w); }
static int in_gold(Lx *x, S w) { return gold_get(x, w).kind != 0; }
static int in_silver(Lx *x, S w) { return silver_get(x, w).kind != 0; }
static S gold_str(Lx *x, const char *w) { Ent e = gold_get(x, mk(w)); return e.kind == 1 ? e.s : NONE_S; }

typedef struct { S ps; int rating; } Res;                  /* ps.n < 0 and rating -1: (None, None) */
#define RES_NONE ((Res){{NULL, -1}, -1})
static Res R(S ps, int rating) { return (Res){ps, rating}; }

/* ================================================================== stress */
static S drop_c(arena *a, S ps, int c) {
    S o = adup(a, ps.p, ps.n); int k = 0;
    for (int i = 0; i < ps.n; i++) if (ps.p[i] != (unsigned char)c) ((unsigned char *)o.p)[k++] = ps.p[i];
    ((unsigned char *)o.p)[k] = 0; o.n = k; return o;
}
static S swap_c(arena *a, S ps, int from, int to) {
    S o = adup(a, ps.p, ps.n);
    for (int i = 0; i < o.n; i++) if (o.p[i] == (unsigned char)from) ((unsigned char *)o.p)[i] = (unsigned char)to;
    return o;
}

static S apply_stress(G *g, S ps, int has_stress, double stress) {
    if (is_none(ps) || !has_stress) return ps;
    arena *a = g->a;
    int has_p = 0, has_s = 0, has_v = 0;
    for (int i = 0; i < ps.n; i++) { has_p |= ps.p[i] == P_PRIMARY; has_s |= ps.p[i] == P_SECONDARY; has_v |= is_vowel(ps.p[i]); }
    if (stress < -1) return drop_c(a, drop_c(a, ps, P_PRIMARY), P_SECONDARY);
    if (stress == -1 || ((stress == 0 || stress == -0.5) && has_p)) return swap_c(a, drop_c(a, ps, P_SECONDARY), P_PRIMARY, P_SECONDARY);
    int lead = 0;
    if ((stress == 0 || stress == 0.5 || stress == 1) && !has_p && !has_s) { if (!has_v) return ps; lead = P_SECONDARY; }
    else if (stress >= 1 && !has_p && has_s) return swap_c(a, ps, P_SECONDARY, P_PRIMARY);
    else if (stress > 1 && !has_p && !has_s) { if (!has_v) return ps; lead = P_PRIMARY; }
    if (!lead) return ps;
    /* restress: the mark moves to just before the first vowel at or after it */
    int n = ps.n + 1;
    unsigned char *t = aalloc(a, (size_t)n + 1); t[0] = (unsigned char)lead; memcpy(t + 1, ps.p, (size_t)ps.n);
    long *key = aalloc(a, sizeof(long) * (size_t)n);
    const long USED = 1L << 60;
    for (int i = 0; i < n; i++) {
        key[i] = 2L * i;
        if (is_stress(t[i])) { int j = i; while (j < n && !is_vowel(t[j])) j++; if (j < n) key[i] = 2L * j - 1; }
    }
    unsigned char *o = aalloc(a, (size_t)n + 1); int k = 0;
    for (int pass = 0; pass < n; pass++) {          /* stable selection by key */
        int best = -1;
        for (int i = 0; i < n; i++) if (key[i] != USED && (best < 0 || key[i] < key[best])) best = i;
        o[k++] = t[best]; key[best] = USED;
    }
    o[k] = 0;
    return (S){o, k};
}

/* ================================================================== numbers */
typedef unsigned long long u64;

static S num_words_under_hundred(arena *a, int v) {
    if (v < 20) return mk(TTS_ONES[v]);
    int tens = v / 10, ones = v % 10;
    if (!ones) return mk(TTS_TENS[tens]);
    return scat(a, scat(a, mk(TTS_TENS[tens]), mk("-")), mk(TTS_ONES[ones]));
}
static S num_words_under_thousand(arena *a, int v) {
    int h = v / 100, rest = v % 100;
    if (!h) return num_words_under_hundred(a, rest);
    S head = scat(a, mk(TTS_ONES[h]), mk(" hundred"));
    return rest ? scat(a, scat(a, head, mk(" and ")), num_words_under_hundred(a, rest)) : head;
}
static S cardinal_words(G *g, u64 value) {
    arena *a = g->a;
    if (value < 1000) return num_words_under_thousand(a, (int)value);
    int groups[8], ng = 0;
    for (u64 r = value; r; r /= 1000) { if (ng >= 6) { fail(g, -1, "number too large to spell"); return mk("zero"); } groups[ng++] = (int)(r % 1000); }
    S out = EMPTY_S; int first = 1, nparts = 0;
    for (int p = ng - 1; p >= 0; p--) {
        if (!groups[p]) continue;
        S part = num_words_under_thousand(a, groups[p]);
        if (p) part = scat(a, scat(a, part, mk(" ")), mk(TTS_SCALES[p]));
        nparts++;
        int last = 1; for (int q = p - 1; q >= 0; q--) if (groups[q]) last = 0;
        if (first) out = part;
        else {
            int tail_and = last && groups[0] > 0 && groups[0] < 100;
            out = scat(a, scat(a, out, mk(tail_and ? " and " : ", ")), part);
        }
        first = 0;
    }
    if (nparts == 1) return out;
    return out;
}
static S ordinalise_word(arena *a, S w) {
    static const char *const from[] = {"one", "two", "three", "five", "eight", "nine", "twelve"};
    static const char *const to[] = {"first", "second", "third", "fifth", "eighth", "ninth", "twelfth"};
    for (int i = 0; i < 7; i++) if (seql(w, from[i])) return mk(to[i]);
    if (w.n && w.p[w.n - 1] == 'y') return scat(a, sub(w, 0, w.n - 1), mk("ieth"));
    return scat(a, w, mk("th"));
}
static S ordinal_words(G *g, u64 value) {
    S words = cardinal_words(g, value);
    int i = words.n; while (i > 0 && islo_a(words.p[i - 1])) i--;
    return scat(g->a, sub(words, 0, i), ordinalise_word(g->a, sub(words, i, words.n)));
}
static S year_words(G *g, u64 value) {
    u64 high = value / 100, low = value % 100;
    if (high == 0 || (high % 10 == 0 && low < 10) || high >= 100) return cardinal_words(g, value);
    if (low == 0) return scat(g->a, cardinal_words(g, high), mk(" hundred"));
    if (low < 10) return scat(g->a, scat(g->a, cardinal_words(g, high), mk(" oh-")), cardinal_words(g, low));
    return scat(g->a, scat(g->a, cardinal_words(g, high), mk(" ")), cardinal_words(g, low));
}
static S decimal_words(G *g, S text) {
    int dot = -1; for (int i = 0; i < text.n; i++) if (text.p[i] == '.') { dot = i; break; }
    S whole = dot < 0 ? text : sub(text, 0, dot), frac = dot < 0 ? EMPTY_S : sub(text, dot + 1, text.n);
    S head = whole.n ? cardinal_words(g, strtoull((const char *)adup(g->a, whole.p, whole.n).p, NULL, 10)) : mk("zero");
    if (!frac.n) return head;
    S out = scat(g->a, head, mk(" point"));
    for (int i = 0; i < frac.n; i++) out = scat(g->a, scat(g->a, out, mk(" ")), mk(TTS_ONES[frac.p[i] - '0']));
    return out;
}
static u64 to_u64(G *g, S digits) {
    if (!all_digits(digits)) { fail(g, -1, "bad number"); return 0; }
    int i = 0; while (i < digits.n - 1 && digits.p[i] == '0') i++;
    if (digits.n - i > 19) { fail(g, -1, "number too large to spell"); return 0; }
    return strtoull((const char *)adup(g->a, digits.p + i, digits.n - i).p, NULL, 10);
}

/* ================================================================== lexicon */
static const char *parent_tag(const char *tag) {
    if (!tag) return NULL;
    if (!strncmp(tag, "VB", 2)) return "VERB";
    if (!strncmp(tag, "NN", 2)) return "NOUN";
    if (!strncmp(tag, "ADV", 3) || !strncmp(tag, "RB", 2)) return "ADV";
    if (!strncmp(tag, "ADJ", 3) || !strncmp(tag, "JJ", 2)) return "ADJ";
    return tag;
}

static Res lookup(Lx *x, S word, const char *tag, int has_stress, double stress, const TokCtx *ctx);
static Res get_NNP(Lx *x, S word);

static S sym_word(S w) {
    if (seql(w, "%")) return mk("percent");
    if (seql(w, "&")) return mk("and");
    if (seql(w, "+")) return mk("plus");
    if (seql(w, "@")) return mk("at");
    return NONE_S;
}

static Res get_NNP(Lx *x, S word) {
    G *g = x->g; arena *a = g->a;
    S spelled = EMPTY_S; int any = 0;
    for (int i = 0; i < word.n; i++) {
        if (!isal(word.p[i])) continue;
        Ent e = gold_get(x, (S){(const unsigned char[]){(unsigned char)upc(word.p[i]), 0}, 1});
        if (e.kind == 0) return RES_NONE;
        S ps = e.kind == 1 ? e.s : EMPTY_S;     /* a tag dict would be a Python error; none of A-Z is one */
        spelled = scat(a, spelled, ps); any = 1;
    }
    if (!any) return RES_NONE;
    spelled = apply_stress(g, spelled, 1, 0);
    int idx = -1; for (int i = spelled.n - 1; i >= 0; i--) if (spelled.p[i] == P_SECONDARY) { idx = i; break; }
    if (idx <= 0) return R(spelled, 3);
    unsigned char mark = P_PRIMARY;
    return R(scat(a, scat(a, sub(spelled, 0, idx), (S){&mark, 1}), sub(spelled, idx + 1, spelled.n)), 3);
}

static Res get_special_case(Lx *x, S word, const char *tag, int has_stress, double stress, const TokCtx *ctx) {
    G *g = x->g; arena *a = g->a;
    if (!strcmp(tag, "ADD") && (seql(word, ".") || seql(word, "/")))
        return lookup(x, mk(seql(word, ".") ? "dot" : "slash"), NULL, 1, -0.5, ctx);
    if (!is_none(sym_word(word))) return lookup(x, sym_word(word), NULL, 0, 0, ctx);
    {   /* "." in word.strip(".") and word.replace(".","").isalpha() and longest dotted part < 3 */
        S stripped = strip_set(word, ".");
        if (has(stripped, '.') && s_isalpha(replace_c(a, word, '.', ""))) {
            int longest = 0, run = 0;
            for (int i = 0; i <= word.n; i++) { if (i == word.n || word.p[i] == '.') { if (run > longest) longest = run; run = 0; } else run++; }
            if (longest < 3) return get_NNP(x, word);
        }
    }
    if (seql(word, "a") || seql(word, "A")) return R(!strcmp(tag, "DT") ? mk("\x98") : scat(a, mk("\x92"), mk("A")), 4);   /* ɐ : ˈA */
    if (seql(word, "am") || seql(word, "Am") || seql(word, "AM")) {
        if (!strncmp(tag, "NN", 2)) return get_NNP(x, word);
        if (ctx->future_vowel == -1 || !seql(word, "am") || (has_stress && stress > 0)) return R(gold_str(x, "am"), 4);
        return R(mk("\x98m"), 4);
    }
    if (seql(word, "an") || seql(word, "An") || seql(word, "AN")) {
        if (seql(word, "AN") && !strncmp(tag, "NN", 2)) return get_NNP(x, word);
        return R(mk("\x98n"), 4);
    }
    if (seql(word, "I") && !strcmp(tag, "PRP")) return R(mk("\x93I"), 4);
    if ((seql(word, "by") || seql(word, "By") || seql(word, "BY")) && !strcmp(parent_tag(tag), "ADV")) return R(mk("b\x92I"), 4);
    if (seql(word, "to") || seql(word, "To") || (seql(word, "TO") && (!strcmp(tag, "TO") || !strcmp(tag, "IN")))) {
        if (ctx->future_vowel == -1) return R(gold_str(x, "to"), 4);
        return R(ctx->future_vowel ? mk("t\x8d") : mk("t\x85"), 4);          /* tʊ : tə */
    }
    if (seql(word, "in") || seql(word, "In") || (seql(word, "IN") && strcmp(tag, "NNP"))) {
        int prefix = ctx->future_vowel == -1 || strcmp(tag, "IN");
        return R(prefix ? mk("\x92\x89n") : mk("\x89n"), 4);                  /* ˈɪn : ɪn */
    }
    if (seql(word, "the") || seql(word, "The") || (seql(word, "THE") && !strcmp(tag, "DT")))
        return R(ctx->future_vowel == 1 ? mk("\x81i") : mk("\x81\x85"), 4);   /* ði : ðə */
    if (!strcmp(tag, "IN") && (word.n == 2 || word.n == 3) && lowc(word.p[0]) == 'v' && lowc(word.p[1]) == 's' && (word.n == 2 || word.p[2] == '.'))
        return lookup(x, mk("versus"), NULL, 0, 0, ctx);
    if (seql(word, "used") || seql(word, "Used") || seql(word, "USED")) {
        Ent e = gold_get(x, mk("used")), d;
        if ((!strcmp(tag, "VBD") || !strcmp(tag, "JJ")) && ctx->future_to) { if (ent_dict_find(&e, "VBD", &d)) return R(d.s, 4); }
        if (ent_dict_find(&e, "DEFAULT", &d)) return R(d.s, 4);
        return R(e.kind == 1 ? e.s : NONE_S, 4);
    }
    return RES_NONE;
}

static int is_known(Lx *x, S word, const char *tag) {
    (void)tag;
    if (in_gold(x, word) || !is_none(sym_word(word)) || in_silver(x, word)) return 1;
    if (!s_isalpha(word)) return 0;
    for (int i = 0; i < word.n; i++) if (is_acc(word.p[i])) return 0;      /* not in LEXICON_ORDS */
    if (word.n == 1) return 1;
    arena *a = x->g->a;
    if (seq(word, supper(a, word)) && in_gold(x, slower(a, word))) return 1;
    return seq(sub(word, 1, word.n), supper(a, sub(word, 1, word.n)));
}

static Res lookup(Lx *x, S word, const char *tag, int has_stress, double stress, const TokCtx *ctx) {
    G *g = x->g; arena *a = g->a;
    int is_nnp = -1;                                        /* None */
    if (seq(word, supper(a, word)) && !in_gold(x, word)) { word = slower(a, word); is_nnp = tag && !strcmp(tag, "NNP"); }
    Ent entry = gold_get(x, word);
    int rating = 4;
    if (entry.kind == 0 && is_nnp != 1) { entry = silver_get(x, word); rating = 3; }
    S ps = NONE_S;
    if (entry.kind == 2) {
        const char *use = tag;
        Ent tmp, got;
        if (ctx && ctx->future_vowel == -1 && ent_dict_find(&entry, "None", &tmp)) use = "None";
        else if (!tag || !ent_dict_find(&entry, tag, &tmp)) use = parent_tag(tag);
        if (use && ent_dict_find(&entry, use, &got)) { entry = got; }
        else if (ent_dict_find(&entry, "DEFAULT", &got)) entry = got;
        else entry.kind = 3;
    }
    if (entry.kind == 1) ps = entry.s;
    int is_none_entry = entry.kind != 1;
    if (is_none_entry || (is_nnp == 1 && memchr(ps.p, P_PRIMARY, (size_t)ps.n) == NULL)) {
        Res sp = get_NNP(x, word);
        if (!is_none(sp.ps)) return sp;
    }
    return R(apply_stress(g, ps, has_stress, stress), rating);
}

static S suffix_s(G *g, S stem) {
    if (is_none(stem) || stem.n == 0) return NONE_S;
    int c = stem.p[stem.n - 1];
    if (c == 'p' || c == 't' || c == 'k' || c == 'f' || c == S_THETA) return scat(g->a, stem, mk("s"));
    if (c == 's' || c == 'z' || c == S_SH || c == S_ZH || c == S_TCH || c == S_DJ) return scat(g->a, scat(g->a, stem, mk("\x96")), mk("z"));
    return scat(g->a, stem, mk("z"));
}
static S suffix_ed(G *g, S stem) {
    if (is_none(stem) || stem.n == 0) return NONE_S;
    int c = stem.p[stem.n - 1];
    if (c == 'p' || c == 'k' || c == 'f' || c == S_THETA || c == S_SH || c == 's' || c == S_TCH) return scat(g->a, stem, mk("t"));
    if (c == 'd') return scat(g->a, scat(g->a, stem, mk("\x96")), mk("d"));
    if (c != 't') return scat(g->a, stem, mk("d"));
    if (stem.n < 2) return scat(g->a, stem, mk("\x89""d"));
    if (is_us_tau(stem.p[stem.n - 2])) return scat(g->a, scat(g->a, sub(stem, 0, stem.n - 1), mk("\x8b\x96")), mk("d"));
    return scat(g->a, scat(g->a, stem, mk("\x96")), mk("d"));
}
static S suffix_ing(G *g, S stem) {
    if (is_none(stem) || stem.n == 0) return NONE_S;
    if (stem.n > 1 && stem.p[stem.n - 1] == 't' && is_us_tau(stem.p[stem.n - 2])) return scat(g->a, sub(stem, 0, stem.n - 1), mk("\x8b\x89\x82"));
    return scat(g->a, stem, mk("\x89\x82"));
}

static Res stem_s(Lx *x, S word, const char *tag, int hs, double st, const TokCtx *ctx) {
    arena *a = x->g->a; S stem;
    if (word.n < 3 || word.p[word.n - 1] != 's') return RES_NONE;
    if (!ends(word, "ss") && is_known(x, sub(word, 0, word.n - 1), tag)) stem = sub(word, 0, word.n - 1);
    else if ((ends(word, "'s") || (word.n > 4 && ends(word, "es") && !ends(word, "ies"))) && is_known(x, sub(word, 0, word.n - 2), tag)) stem = sub(word, 0, word.n - 2);
    else if (word.n > 4 && ends(word, "ies") && is_known(x, scat(a, sub(word, 0, word.n - 3), mk("y")), tag)) stem = scat(a, sub(word, 0, word.n - 3), mk("y"));
    else return RES_NONE;
    Res r = lookup(x, stem, tag, hs, st, ctx);
    return R(suffix_s(x->g, r.ps), r.rating);
}
static Res stem_ed(Lx *x, S word, const char *tag, int hs, double st, const TokCtx *ctx) {
    S stem;
    if (word.n < 4 || word.p[word.n - 1] != 'd') return RES_NONE;
    if (!ends(word, "dd") && is_known(x, sub(word, 0, word.n - 1), tag)) stem = sub(word, 0, word.n - 1);
    else if (word.n > 4 && ends(word, "ed") && !ends(word, "eed") && is_known(x, sub(word, 0, word.n - 2), tag)) stem = sub(word, 0, word.n - 2);
    else return RES_NONE;
    Res r = lookup(x, stem, tag, hs, st, ctx);
    return R(suffix_ed(x->g, r.ps), r.rating);
}
static int doubled_or_cking(S w) {   /* re.search(r"([bcdgklmnprstvxz])\1ing$|cking$", word) */
    if (ends(w, "cking")) return 1;
    if (w.n >= 5 && ends(w, "ing")) { int c = w.p[w.n - 4], d = w.p[w.n - 5]; if (c == d && c && strchr("bcdgklmnprstvxz", c)) return 1; }
    return 0;
}
static Res stem_ing(Lx *x, S word, const char *tag, int hs, double st, const TokCtx *ctx) {
    arena *a = x->g->a; S stem;
    if (word.n < 5 || !ends(word, "ing")) return RES_NONE;
    if (word.n > 5 && is_known(x, sub(word, 0, word.n - 3), tag)) stem = sub(word, 0, word.n - 3);
    else if (is_known(x, scat(a, sub(word, 0, word.n - 3), mk("e")), tag)) stem = scat(a, sub(word, 0, word.n - 3), mk("e"));
    else if (word.n > 5 && doubled_or_cking(word) && is_known(x, sub(word, 0, word.n - 4), tag)) stem = sub(word, 0, word.n - 4);
    else return RES_NONE;
    Res r = lookup(x, stem, tag, hs, st, ctx);
    return R(suffix_ing(x->g, r.ps), r.rating);
}

static int res_truthy(Res r) { return !is_none(r.ps) && r.ps.n > 0; }

static Res get_word(Lx *x, S word, const char *tag, int has_stress, double stress, const TokCtx *ctx) {
    arena *a = x->g->a;
    Res sp = get_special_case(x, word, tag ? tag : "", has_stress, stress, ctx);
    if (!is_none(sp.ps)) return sp;
    S lowered = slower(a, word);
    S nq = replace_c(a, word, '\'', "");
    if (word.n > 1 && s_isalpha(nq) && !seq(word, lowered) && (!tag || strcmp(tag, "NNP") || word.n > 7)
        && !in_gold(x, word) && !in_silver(x, word)
        && (seq(word, supper(a, word)) || seq(sub(word, 1, word.n), slower(a, sub(word, 1, word.n))))
        && (in_gold(x, lowered) || in_silver(x, lowered)
            || res_truthy(stem_s(x, lowered, tag, has_stress, stress, ctx))
            || res_truthy(stem_ed(x, lowered, tag, has_stress, stress, ctx))
            || res_truthy(stem_ing(x, lowered, tag, has_stress, stress, ctx))))
        word = lowered;
    if (is_known(x, word, tag)) return lookup(x, word, tag, has_stress, stress, ctx);
    if (ends(word, "s'") && is_known(x, scat(a, sub(word, 0, word.n - 2), mk("'s")), tag))
        return lookup(x, scat(a, sub(word, 0, word.n - 2), mk("'s")), tag, has_stress, stress, ctx);
    if (ends(word, "'") && is_known(x, sub(word, 0, word.n - 1), tag))
        return lookup(x, sub(word, 0, word.n - 1), tag, has_stress, stress, ctx);
    Res r = stem_s(x, word, tag, has_stress, stress, ctx);
    if (!is_none(r.ps)) return r;
    r = stem_ed(x, word, tag, has_stress, stress, ctx);
    if (!is_none(r.ps)) return r;
    int ing_has = 1; double ing_st = has_stress ? stress : 0.5;
    r = stem_ing(x, word, tag, ing_has, ing_st, ctx);
    if (!is_none(r.ps)) return r;
    return RES_NONE;
}

static int is_currency_word(S word) {
    if (!has(word, '.')) return 1;
    if (count_c(word, '.') > 1) return 0;
    int dot = 0; while (word.p[dot] != '.') dot++;
    S cents = sub(word, dot + 1, word.n);
    if (cents.n < 3) return 1;
    for (int i = 0; i < cents.n; i++) if (cents.p[i] != '0') return 0;
    return 1;
}

static int is_number_word(S word, int is_head) {
    int any = 0; for (int i = 0; i < word.n; i++) if (isdig(word.p[i])) any = 1;
    if (!any) return 0;
    static const char *const sufs[] = {"ing", "'d", "ed", "'s", "st", "nd", "rd", "th", "s"};
    for (int k = 0; k < 9; k++) if (ends(word, sufs[k])) { word = sub(word, 0, word.n - (int)strlen(sufs[k])); break; }
    for (int i = 0; i < word.n; i++) {
        int c = word.p[i];
        if (!(isdig(c) || c == ',' || c == '.' || (is_head && i == 0 && c == '-'))) return 0;
    }
    return 1;
}

typedef struct { S ps; int rating; } Part;

static Res get_number(Lx *x, S word, unsigned char currency, int is_head, const char *flags) {
    G *g = x->g; arena *a = g->a;
    Part *res = aalloc(a, sizeof(Part) * (size_t)(word.n * 8 + 64)); int nres = 0;
    int suf_start = word.n; while (suf_start > 0 && (islo_a(word.p[suf_start - 1]) || word.p[suf_start - 1] == '\'')) suf_start--;
    S suffix = suf_start < word.n ? sub(word, suf_start, word.n) : NONE_S;
    if (!is_none(suffix)) word = sub(word, 0, suf_start);
    int suf_ord = !is_none(suffix) && (seql(suffix, "st") || seql(suffix, "nd") || seql(suffix, "rd") || seql(suffix, "th"));
    int amp = strchr(flags, '&') != NULL, fa = strchr(flags, 'a') != NULL, fn_ = strchr(flags, 'n') != NULL;
    if (word.n && word.p[0] == '-') { Res m = lookup(x, mk("minus"), NULL, 0, 0, NULL); res[nres++] = (Part){m.ps, m.rating}; word = sub(word, 1, word.n); }

    #define EXTEND(numtxt, firstflag, escape) do { \
        S spelled_ = (escape) ? (numtxt) : cardinal_words(g, to_u64(g, (numtxt))); \
        int nparts_ = 0, idx_ = 0; \
        for (int i_ = 0; i_ <= spelled_.n; i_++) { if (i_ == spelled_.n || !islo_a(spelled_.p[i_])) { if (i_ > 0 && islo_a(spelled_.p[i_ - 1])) nparts_++; } } \
        int p0_ = 0; \
        for (int i_ = 0; i_ <= spelled_.n; i_++) { \
            if (i_ < spelled_.n && islo_a(spelled_.p[i_])) continue; \
            S part_ = sub(spelled_, p0_, i_); p0_ = i_ + 1; \
            if (!part_.n) continue; \
            int first_part_ = idx_ == 0; idx_++; \
            if (!seql(part_, "and") || amp) { \
                if ((firstflag) && first_part_ && nparts_ > 1 && seql(part_, "one") && fa) res[nres++] = (Part){mk("\x85"), 4}; \
                else { int pt_ = seql(part_, "point"); Res r_ = lookup(x, part_, NULL, pt_, pt_ ? -2.0 : 0, NULL); res[nres++] = (Part){r_.ps, r_.rating}; } \
            } else if (seql(part_, "and") && fn_ && nres) { \
                S prev_ = is_none(res[nres - 1].ps) ? EMPTY_S : res[nres - 1].ps; \
                res[nres - 1].ps = scat(a, prev_, mk("\x85n")); \
            } \
        } \
    } while (0)

    if (all_digits(word) && suf_ord) {
        S sp = ordinal_words(g, to_u64(g, word));
        EXTEND(sp, 1, 1);
    } else if (!nres && word.n == 4 && currency != '$' && currency != S_POUND && currency != S_EURO && all_digits(word)) {
        S sp = year_words(g, to_u64(g, word));
        EXTEND(sp, 1, 1);
    } else if (!is_head && !has(word, '.')) {
        S num = replace_c(a, word, ',', "");
        if (num.n && (num.p[0] == '0' || num.n > 3)) {
            for (int i = 0; i < num.n; i++) { S d = sub(num, i, i + 1); EXTEND(d, 0, 0); }
        } else if (num.n == 3 && !ends(num, "00")) {
            S d0 = sub(num, 0, 1); EXTEND(d0, 1, 0);
            if (num.p[1] == '0') {
                Res o = lookup(x, mk("O"), NULL, 1, -2.0, NULL); res[nres++] = (Part){o.ps, o.rating};
                S d2 = sub(num, 2, 3); EXTEND(d2, 0, 0);
            } else { S d12 = sub(num, 1, 3); EXTEND(d12, 0, 0); }
        } else { EXTEND(num, 1, 0); }
    } else if (count_c(word, '.') > 1 || !is_head) {
        int first = 1; S all = replace_c(a, word, ',', ""); int p0 = 0;
        for (int i = 0; i <= all.n; i++) {
            if (i < all.n && all.p[i] != '.') continue;
            S num = sub(all, p0, i); p0 = i + 1;
            if (!num.n) { first = 0; continue; }
            int nz = 0; for (int k = 1; k < num.n; k++) if (num.p[k] != '0') nz = 1;
            if (num.p[0] == '0' || (num.n != 2 && nz)) { for (int k = 0; k < num.n; k++) { S d = sub(num, k, k + 1); EXTEND(d, 0, 0); } }
            else EXTEND(num, first, 0);
            first = 0;
        }
    } else if ((currency == '$' || currency == S_POUND || currency == S_EURO) && is_currency_word(word)) {
        const char *unit[2]; unit[0] = currency == '$' ? "dollar" : currency == S_POUND ? "pound" : "euro";
        unit[1] = currency == S_POUND ? "pence" : "cent";
        S all = replace_c(a, word, ',', ""); S parts[2]; int np = 0, p0 = 0;
        for (int i = 0; i <= all.n && np < 2; i++) { if (i < all.n && all.p[i] != '.') continue; parts[np++] = sub(all, p0, i); p0 = i + 1; }
        u64 amt[2]; for (int i = 0; i < np; i++) amt[i] = parts[i].n ? to_u64(g, parts[i]) : 0;
        int first_idx = 0;
        if (np > 1) { if (amt[1] == 0) np = 1; else if (amt[0] == 0) first_idx = 1; }
        for (int i = first_idx; i < np; i++) {
            if (i > first_idx) { Res an = lookup(x, mk("and"), NULL, 0, 0, NULL); res[nres++] = (Part){an.ps, an.rating}; }
            char buf[32]; snprintf(buf, sizeof buf, "%llu", amt[i]);
            S nums = adup(a, (const unsigned char *)buf, (int)strlen(buf));
            EXTEND(nums, i == first_idx, 0);
            Res u = (amt[i] != 1 && strcmp(unit[i], "pence")) ? stem_s(x, scat(a, mk(unit[i]), mk("s")), NULL, 0, 0, NULL) : lookup(x, mk(unit[i]), NULL, 0, 0, NULL);
            res[nres++] = (Part){u.ps, u.rating};
        }
    } else {
        S cleaned = replace_c(a, word, ',', ""); S spelled;
        if (all_digits(cleaned)) { u64 v = to_u64(g, cleaned); spelled = suf_ord ? ordinal_words(g, v) : cardinal_words(g, v); }
        else if (!has(cleaned, '.')) { fail(g, -1, "cannot spell this as a number"); return RES_NONE; }
        else if (cleaned.p[0] == '.') {
            spelled = mk("point");
            for (int i = 1; i < cleaned.n; i++) spelled = scat(a, scat(a, spelled, mk(" ")), mk(TTS_ONES[cleaned.p[i] - '0']));
        } else spelled = decimal_words(g, cleaned);
        EXTEND(spelled, 1, 1);
    }
    #undef EXTEND
    if (!nres) return RES_NONE;
    S joined = EMPTY_S; int firstp = 1, rating = -1;
    for (int i = 0; i < nres; i++) {
        if (!is_none(res[i].ps) && res[i].ps.n) { joined = firstp ? res[i].ps : scat(a, scat(a, joined, mk(" ")), res[i].ps); firstp = 0; }
        if (res[i].rating >= 0 && (rating < 0 || res[i].rating < rating)) rating = res[i].rating;
    }
    if (!is_none(suffix)) {
        if (seql(suffix, "s") || seql(suffix, "'s")) return R(suffix_s(g, joined), rating);
        if (seql(suffix, "ed") || seql(suffix, "'d")) return R(suffix_ed(g, joined), rating);
        if (seql(suffix, "ing")) return R(suffix_ing(g, joined), rating);
    }
    return R(joined, rating);
}

static S append_currency(Lx *x, S ps, unsigned char currency) {
    if (!currency) return ps;
    const char *unit = currency == '$' ? "dollar" : currency == S_POUND ? "pound" : currency == S_EURO ? "euro" : NULL;
    if (!unit) return ps;
    Res u = stem_s(x, scat(x->g->a, mk(unit), mk("s")), NULL, 0, 0, NULL);
    if (is_none(u.ps) || !u.ps.n) return ps;
    return scat(x->g->a, scat(x->g->a, ps, mk(" ")), u.ps);
}

/* Lexicon.__call__ */
static Res lexicon_call(Lx *x, const Tok *tok, const TokCtx *ctx) {
    G *g = x->g; arena *a = g->a;
    S word = tok->has_alias ? tok->alias : tok->text;
    word = replace_c(a, replace_c(a, word, S_LSQUOTE, "'"), S_RSQUOTE, "'");
    int has_stress = !seq(word, slower(a, word)), stress = 0;
    if (has_stress) stress = seq(word, supper(a, word)) ? 2 : 0;   /* cap_stresses = (0.5, 2) */
    double st = has_stress ? (stress == 2 ? 2.0 : 0.5) : 0;
    Res r = get_word(x, word, tok->tag, has_stress, st, ctx);
    if (!is_none(r.ps))
        return R(apply_stress(g, append_currency(x, r.ps, tok->currency), tok->has_stress, tok->stress), r.rating);
    if (is_number_word(word, tok->is_head)) {
        r = get_number(x, word, tok->currency, tok->is_head, tok->flags);
        return R(apply_stress(g, r.ps, tok->has_stress, tok->stress), r.rating);
    }
    return RES_NONE;
}

/* ================================================================== input normalisation */
int tts_decode_utf8(const unsigned char *s, size_t n, size_t *adv) {
    unsigned cp; size_t k = 1;
    if (s[0] < 0x80) { *adv = 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && n >= 2 && (s[1] & 0xC0) == 0x80) { cp = (unsigned)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); k = 2; }
    else if ((s[0] & 0xF0) == 0xE0 && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { cp = (unsigned)(s[0] & 0x0F) << 12 | (unsigned)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); k = 3; }
    else if ((s[0] & 0xF8) == 0xF0 && n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) { cp = (unsigned)(s[0] & 0x07) << 18 | (unsigned)(s[1] & 0x3F) << 12 | (unsigned)(s[2] & 0x3F) << 6 | (s[3] & 0x3F); k = 4; }
    else { *adv = 1; return TTS_OTHER; }
    *adv = k;
    /* whitespace beyond ASCII */
    if (cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000) return ' ';
    if (cp >= 0xC0 && cp < 0x180 && TTS_LATIN_FOLD[cp - 0xC0]) { int b = TTS_LATIN_FOLD[cp - 0xC0]; return b >= 'a' ? ACC_LO + (b - 'a') : ACC_UP + (b - 'A'); }
    static const struct { unsigned cp; int code; } sym[] = {
        {0xE6, S_AE}, {0xF0, S_ETH}, {0x14B, S_ENG}, {0x251, S_OPENA}, {0x254, S_OPENO}, {0x259, S_SCHWA}, {0x25B, S_OPENE},
        {0x25C, S_REVE}, {0x261, S_G}, {0x26A, S_IH}, {0x279, S_R}, {0x27E, S_TAP}, {0x283, S_SH}, {0x28A, S_UH}, {0x28C, S_WEDGE},
        {0x292, S_ZH}, {0x2A4, S_DJ}, {0x2A7, S_TCH}, {0x2C8, S_PRIMARY}, {0x2CC, S_SECONDARY}, {0x3B8, S_THETA}, {0x1D4A, S_SCHWAS},
        {0x1D7B, S_IBAR}, {0x294, S_GLOTTAL}, {0x250, S_AEXT}, {0x2014, S_EMDASH}, {0x201C, S_LDQUOTE}, {0x201D, S_RDQUOTE},
        {0x2026, S_ELLIPSIS}, {0x2018, S_LSQUOTE}, {0x2019, S_RSQUOTE}, {0xAB, S_LGUIL}, {0xBB, S_RGUIL}, {0xBF, S_IQUEST},
        {0xA1, S_IEXCL}, {0xA3, S_POUND}, {0x20AC, S_EURO}, {0x2013, S_ENDASH}};
    for (size_t i = 0; i < sizeof sym / sizeof sym[0]; i++) if (sym[i].cp == cp) return sym[i].code;
    return TTS_OTHER;
}

static S normalize_input(arena *a, const char *text) {
    size_t n = strlen(text);
    unsigned char *d = aalloc(a, n + 1); size_t k = 0;
    for (size_t i = 0; i < n;) { size_t adv; int c = tts_decode_utf8((const unsigned char *)text + i, n - i, &adv); i += adv; d[k++] = (unsigned char)c; }
    d[k] = 0;
    return (S){d, (int)k};
}

static int is_space_c(int c) { return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F); }

/* ================================================================== tokeniser + tagger */
static int closed_tag(S lowered, const char **tag) {
    for (size_t i = 0; i < sizeof TTS_CLOSED_CLASS / sizeof TTS_CLOSED_CLASS[0]; i++)
        if (seql(lowered, TTS_CLOSED_CLASS[i].word)) { *tag = TTS_CLOSED_CLASS[i].tag; return 1; }
    return 0;
}
static int is_abbrev(S w) {
    for (size_t i = 0; i < sizeof TTS_ABBREVIATIONS / sizeof TTS_ABBREVIATIONS[0]; i++) if (seql(w, TTS_ABBREVIATIONS[i])) return 1;
    return 0;
}
static int punct_tag_of(int c, const char **tag) {
    for (size_t i = 0; i < sizeof TTS_PUNCT_TAG / sizeof TTS_PUNCT_TAG[0]; i++) if ((unsigned char)TTS_PUNCT_TAG[i].word[0] == c) { *tag = TTS_PUNCT_TAG[i].tag; return 1; }
    return 0;
}
static int is_prefix_ch(int c) { return strchr("\"`([{$#*", c) != NULL ? c != 0 : (c == S_LDQUOTE || c == S_LSQUOTE || c == S_LGUIL || c == S_IQUEST || c == S_IEXCL || c == S_POUND || c == S_EURO || c == S_ELLIPSIS); }
static int is_suffix_ch(int c) { return c != 0 && strchr(".,;:!?\")]}%", c) != NULL ? 1 : (c == S_RDQUOTE || c == S_RGUIL || c == S_ELLIPSIS); }
static int is_word_ch(int c) { return isal(c) || isdig(c) || c == '_'; }

static int keeps_final_period(arena *a, S word) {
    if (!(word.n && word.p[word.n - 1] == '.')) return 0;
    if (is_abbrev(word) || is_abbrev(slower(a, word))) return 1;
    S stripped = strip_set(word, ".");
    if (!has(stripped, '.') || !s_isalpha(replace_c(a, word, '.', ""))) return 0;
    int longest = 0, run = 0;
    for (int i = 0; i <= word.n; i++) { if (i == word.n || word.p[i] == '.') { if (run > longest) longest = run; run = 0; } else run++; }
    return longest < 3;
}

typedef struct { S s; int pos; } Piece;               /* pos: 0 prefix, 1 core, 2 suffix */

static int split_chunk(arena *a, S chunk, Piece *out) {
    if (!chunk.n) return 0;
    int *pre = aalloc(a, sizeof(int) * (size_t)chunk.n), *suf = aalloc(a, sizeof(int) * (size_t)chunk.n), np = 0, ns = 0;
    S core = chunk;
    int changed = 1;
    while (changed && core.n > 1) {
        changed = 0;
        if (is_prefix_ch(core.p[0])) { pre[np++] = core.p[0]; core = sub(core, 1, core.n); changed = 1; continue; }
        if (is_suffix_ch(core.p[core.n - 1]) && !keeps_final_period(a, core)) { suf[ns++] = core.p[core.n - 1]; core = sub(core, 0, core.n - 1); changed = 1; }
    }
    int k = 0;
    for (int i = 0; i < np; i++) out[k++] = (Piece){adup(a, (const unsigned char[]){(unsigned char)pre[i]}, 1), 0};
    /* split core on runs of 2+ hyphens that sit between two word characters (the captured runs are kept) */
    int start = 0;
    for (int i = 1; i < core.n; i++) {
        if (core.p[i] == '-' && is_word_ch(core.p[i - 1]) && core.p[i - 1] != '-') {
            int j = i; while (j < core.n && core.p[j] == '-') j++;
            if (j - i >= 2 && j < core.n && is_word_ch(core.p[j])) {
                if (i > start) out[k++] = (Piece){sub(core, start, i), 1};
                out[k++] = (Piece){sub(core, i, j), 1};
                start = j; i = j - 1;
            } else i = j - 1;
        }
    }
    if (core.n > start) out[k++] = (Piece){sub(core, start, core.n), 1};
    for (int i = ns - 1; i >= 0; i--) out[k++] = (Piece){adup(a, (const unsigned char[]){(unsigned char)suf[i]}, 1), 2};
    return k;
}

static const char *punct_tag(S text, int position, int quote_open) {
    for (int i = 0; i < text.n; i++) if (isal(text.p[i]) || isdig(text.p[i])) return NULL;
    if (!is_none(sym_word(text))) return "NN";
    int all_q = text.n > 0;
    for (int i = 0; i < text.n; i++) if (!(text.p[i] == '"' || text.p[i] == S_LDQUOTE || text.p[i] == S_RDQUOTE)) all_q = 0;
    if (all_q) {
        if (text.n == 1 && text.p[0] == S_LDQUOTE) return "``";
        if (text.n == 1 && text.p[0] == S_RDQUOTE) return "''";
        if (position == 0) return "``";
        if (position == 2) return "''";
        return quote_open ? "``" : "''";
    }
    int all_apos = text.n > 0; for (int i = 0; i < text.n; i++) if (text.p[i] != '\'') all_apos = 0;
    if (all_apos) return "''";
    const char *only = NULL; int uniform = 1, first = 1;
    for (int i = 0; i < text.n; i++) {
        const char *t = NULL; punct_tag_of(text.p[i], &t);
        if (first) { only = t; first = 0; } else if ((only == NULL) != (t == NULL) || (only && t && strcmp(only, t))) uniform = 0;
    }
    if (uniform && only) return only;
    return "NFP";
}

static void tag_token(arena *a, S text, int position, int sentence_start, int quote_open, char out[8]) {
    const char *pt = punct_tag(text, position, quote_open);
    if (pt) { snprintf(out, 8, "%s", pt); return; }
    if (is_number_word(text, 1)) { snprintf(out, 8, "CD"); return; }
    if (seql(text, "I")) { snprintf(out, 8, "PRP"); return; }
    const char *cc;
    if (closed_tag(slower(a, text), &cc)) { snprintf(out, 8, "%s", cc); return; }
    if (text.n && isup(text.p[0]) && !sentence_start) { snprintf(out, 8, "NNP"); return; }
    snprintf(out, 8, "NN");
}

static Tok *new_tok(arena *a) {
    Tok *t = aalloc(a, sizeof *t); memset(t, 0, sizeof *t);
    t->text = EMPTY_S; t->ws = EMPTY_S; t->ph = NONE_S; t->alias = NONE_S; t->rating = -1; t->is_head = 1;
    return t;
}

static int in_list(const char *tag, const char *const *list) { for (; *list; list++) if (!strcmp(tag, *list)) return 1; return 0; }

static void retag_verbs(arena *a, Tok **tk, int n) {
    static const char *const aux[] = {"have", "has", "had", "having", "is", "are", "was", "were", "be", "been", "being", NULL};
    char prev_tag[8] = ""; int have_prev = 0; S prev_text = EMPTY_S;
    for (int i = 0; i < n; i++) {
        Tok *t = tk[i];
        if (tag_is(t, "NN") || tag_is(t, "JJ")) {
            if (have_prev && (!strcmp(prev_tag, "TO") || !strcmp(prev_tag, "MD"))) set_tag(t, "VB");
            else { for (const char *const *p = aux; *p; p++) if (seql(prev_text, *p)) { set_tag(t, "VBN"); break; } }
        }
        if (!tag_is(t, "RB") && !tag_is(t, "RBR") && !tag_is(t, "RBS")) { snprintf(prev_tag, 8, "%s", t->tag); have_prev = 1; prev_text = slower(a, t->text); }
    }
}
static void retag_that(arena *a, Tok **tk, int n) {
    for (int i = 0; i < n; i++) {
        if (!seql(slower(a, tk[i]->text), "that") || !tag_is(tk[i], "IN")) continue;
        if (i + 1 < n && (!strncmp(tk[i + 1]->tag, "NN", 2) || !strncmp(tk[i + 1]->tag, "JJ", 2))) set_tag(tk[i], "DT");
    }
}

static int tokenize(arena *a, S text, Tok **out) {
    int n = 0, quote_open = 1, sentence_start = 1;
    int i = 0;
    Piece *pcs = aalloc(a, sizeof(Piece) * (size_t)(text.n + 4));
    while (i < text.n) {
        while (i < text.n && is_space_c(text.p[i])) i++;
        if (i >= text.n) break;
        int j = i; while (j < text.n && !is_space_c(text.p[j])) j++;
        int np = split_chunk(a, sub(text, i, j), pcs);
        for (int k = 0; k < np; k++) {
            Tok *t = new_tok(a);
            t->text = pcs[k].s;
            tag_token(a, pcs[k].s, pcs[k].pos, sentence_start, quote_open, t->tag);
            if (tag_is(t, "``") || tag_is(t, "''")) quote_open = tag_is(t, "''");
            sentence_start = tag_is(t, ".") || (sentence_start && (tag_is(t, "``") || tag_is(t, "-LRB-")));
            t->ws = k == np - 1 ? mk(" ") : EMPTY_S;
            out[n++] = t;
        }
        i = j;
    }
    if (n) out[n - 1]->ws = EMPTY_S;
    retag_verbs(a, out, n);
    retag_that(a, out, n);
    return n;
}

/* ================================================================== sub-tokeniser (SUBTOKEN_REGEX.findall) */
static int is_letter(int c) { return isal(c); }

static int subtok_match(S w, int p, int *end) {
    const int n = w.n; const unsigned char *s = w.p;
    /* 1: ^['‘’]+ */
    if (p == 0 && is_quote_ch(s[0])) { int q = 0; while (q < n && is_quote_ch(s[q])) q++; *end = q; return 1; }
    /* 2: [A-Z](?=[A-Z][a-z]) */
    if (isup_a(s[p]) && p + 2 < n && isup_a(s[p + 1]) && islo_a(s[p + 2])) { *end = p + 1; return 1; }
    /* 3: (?:^-)?(?:\d?[,.]?\d)+ */
    {
        int q = p;
        if (p == 0 && s[0] == '-') q = 1;
        int groups = 0;
        for (;;) {
            int len = 0;
            if (q < n && isdig(s[q])) {
                if (q + 2 < n && (s[q + 1] == ',' || s[q + 1] == '.') && isdig(s[q + 2])) len = 3;
                else if (q + 1 < n && isdig(s[q + 1])) len = 2;
                else len = 1;
            } else if (q + 1 < n && (s[q] == ',' || s[q] == '.') && isdig(s[q + 1])) len = 2;
            if (!len) break;
            q += len; groups++;
        }
        if (groups) { *end = q; return 1; }
    }
    /* 4: [-_]+ */
    if (s[p] == '-' || s[p] == '_') { int q = p; while (q < n && (s[q] == '-' || s[q] == '_')) q++; *end = q; return 1; }
    /* 5: ['‘’]{2,} */
    if (is_quote_ch(s[p])) { int q = p; while (q < n && is_quote_ch(s[q])) q++; if (q - p >= 2) { *end = q; return 1; } }
    /* 6: LETTER*?(?:['‘’]LETTER)*?[a-z](?=[A-Z]) */
    for (int k = 0; ; k++) {
        if (p + k > n) break;
        int ok = 1; for (int i = 0; i < k; i++) if (!is_letter(s[p + i])) { ok = 0; break; }
        if (!ok) break;
        int q = p + k;
        for (;;) {
            if (q + 1 < n && islo_a(s[q]) && isup_a(s[q + 1])) { *end = q + 1; return 1; }
            if (q + 1 < n && is_quote_ch(s[q]) && is_letter(s[q + 1])) { q += 2; continue; }
            break;
        }
    }
    /* 7: LETTER+(?:['‘’]LETTER)* */
    if (is_letter(s[p])) {
        int q = p; while (q < n && is_letter(s[q])) q++;
        while (q + 1 < n && is_quote_ch(s[q]) && is_letter(s[q + 1])) { q += 2; while (q < n && is_letter(s[q])) q++; }
        *end = q; return 1;
    }
    /* 8: [^\w'‘’-] */
    if (!is_word_ch(s[p]) && !is_quote_ch(s[p]) && s[p] != '-') { *end = p + 1; return 1; }
    /* 9: ['‘’]+$ */
    if (is_quote_ch(s[p])) { int q = p; while (q < n && is_quote_ch(s[q])) q++; if (q == n) { *end = q; return 1; } }
    return 0;
}

static int subtokenize(S w, S *out) {
    int n = 0, p = 0;
    while (p < w.n) {
        int e;
        if (subtok_match(w, p, &e)) { out[n++] = sub(w, p, e); p = e > p ? e : p + 1; } else p++;
    }
    return n;
}

/* ================================================================== merge / retokenize */
static Tok *copy_tok(arena *a, const Tok *t) { Tok *c = aalloc(a, sizeof *c); *c = *t; return c; }

static Tok *merge_tokens(arena *a, Tok **tk, int n, int has_unk) {
    Tok *m = new_tok(a);
    unsigned char cur = 0; int all_rating = 1, minr = 1 << 30;
    char flags[64] = ""; int nf = 0;
    for (int i = 0; i < n; i++) {
        if (tk[i]->currency > cur) cur = tk[i]->currency;
        if (tk[i]->rating < 0) all_rating = 0; else if (tk[i]->rating < minr) minr = tk[i]->rating;
        for (const char *f = tk[i]->flags; *f; f++) if (!strchr(flags, *f) && nf < 60) { flags[nf++] = *f; flags[nf] = 0; }
    }
    /* stress: the single distinct value, else None */
    { double first = 0; int have = 0, distinct = 0;
      for (int i = 0; i < n; i++) if (tk[i]->has_stress) { if (!have) { first = tk[i]->stress; have = 1; distinct = 1; } else if (tk[i]->stress != first) distinct = 2; }
      m->has_stress = have && distinct == 1; m->stress = first; }
    S text = EMPTY_S;
    for (int i = 0; i < n - 1; i++) text = scat(a, scat(a, text, tk[i]->text), tk[i]->ws);
    m->text = scat(a, text, tk[n - 1]->text);
    int best = 0, bestw = -1;
    for (int i = 0; i < n; i++) { int w = 0; for (int k = 0; k < tk[i]->text.n; k++) w += isup(tk[i]->text.p[k]) ? 2 : 1; if (w > bestw) { bestw = w; best = i; } }
    snprintf(m->tag, sizeof m->tag, "%s", tk[best]->tag);
    m->ws = tk[n - 1]->ws;
    m->is_head = tk[0]->is_head; m->prespace = tk[0]->prespace;
    m->currency = cur;
    /* sorted flags */
    for (int i = 0; i < nf; i++) for (int j = i + 1; j < nf; j++) if (flags[j] < flags[i]) { char t = flags[i]; flags[i] = flags[j]; flags[j] = t; }
    snprintf(m->flags, sizeof m->flags, "%s", flags);
    m->rating = all_rating ? minr : -1;
    if (has_unk) {
        S ph = EMPTY_S;
        for (int i = 0; i < n; i++) {
            if (tk[i]->prespace && ph.n && ph.p[ph.n - 1] != ' ' && !is_none(tk[i]->ph) && tk[i]->ph.n) ph = scat(a, ph, mk(" "));
            if (!is_none(tk[i]->ph)) ph = scat(a, ph, tk[i]->ph);
        }
        m->ph = ph;
    }
    return m;
}

typedef struct { Tok **t; int n; int is_list; } Word;

static int retokenize(arena *a, Tok **tokens, int ntok, Word *words) {
    int nw = 0; unsigned char currency = 0, total = 0; (void)total;
    int cap = ntok + 8, maxlen = 8;
    for (int i = 0; i < ntok; i++) { cap += tokens[i]->text.n; if (tokens[i]->text.n > maxlen) maxlen = tokens[i]->text.n; }
    S *pieces = aalloc(a, sizeof(S) * (size_t)(maxlen + 8));
    for (int i = 0; i < ntok; i++) {
        Tok *token = tokens[i];
        Tok **tks; int ntks;
        if (!token->has_alias && is_none(token->ph)) {
            int np = subtokenize(token->text, pieces);
            if (!np) { pieces[0] = token->text; np = 1; }
            tks = aalloc(a, sizeof(Tok *) * (size_t)np);
            for (int k = 0; k < np; k++) {
                Tok *c = copy_tok(a, token);
                c->text = pieces[k]; c->ws = EMPTY_S; c->is_head = 1; c->prespace = 0; c->ph = NONE_S; c->has_alias = 0; c->alias = NONE_S; c->currency = 0; c->rating = -1;
                tks[k] = c;
            }
            ntks = np;
        } else { tks = aalloc(a, sizeof(Tok *)); tks[0] = token; ntks = 1; }
        tks[ntks - 1]->ws = token->ws;
        for (int j = 0; j < ntks; j++) {
            Tok *tk = tks[j];
            if (tk->has_alias || !is_none(tk->ph)) { /* pass */ }
            else if (tag_is(tk, "$") && tk->text.n == 1 && (tk->text.p[0] == '$' || tk->text.p[0] == S_POUND || tk->text.p[0] == S_EURO)) {
                currency = tk->text.p[0]; tk->ph = EMPTY_S; tk->rating = 4;
            } else if (tag_is(tk, ":") && (seql(tk->text, "-") || (tk->text.n == 1 && tk->text.p[0] == S_ENDASH))) {
                unsigned char dash = S_EMDASH; tk->ph = adup(a, &dash, 1); tk->rating = 3;
            } else if ((tag_is(tk, ".") || tag_is(tk, ",") || tag_is(tk, "-LRB-") || tag_is(tk, "-RRB-") || tag_is(tk, "``") || tag_is(tk, "\"\"") || tag_is(tk, "''")
                        || tag_is(tk, ":") || tag_is(tk, "$") || tag_is(tk, "#") || tag_is(tk, "NFP"))
                       && !({ int allal = 1; for (int q = 0; q < tk->text.n; q++) if (!isal(tk->text.p[q])) allal = 0; allal; })) {
                unsigned char one;
                if (tag_is(tk, "-LRB-")) { one = '('; tk->ph = adup(a, &one, 1); }
                else if (tag_is(tk, "-RRB-")) { one = ')'; tk->ph = adup(a, &one, 1); }
                else if (tag_is(tk, "``")) { one = S_LDQUOTE; tk->ph = adup(a, &one, 1); }
                else if (tag_is(tk, "\"\"") || tag_is(tk, "''")) { one = S_RDQUOTE; tk->ph = adup(a, &one, 1); }
                else {
                    unsigned char *d = aalloc(a, (size_t)tk->text.n + 1); int k = 0;
                    for (int q = 0; q < tk->text.n; q++) if (is_punct(tk->text.p[q])) d[k++] = tk->text.p[q];
                    d[k] = 0; tk->ph = (S){d, k};
                }
                tk->rating = 4;
            } else if (currency) {
                if (!tag_is(tk, "CD")) currency = 0;
                else if (j + 1 == ntks && (i + 1 == ntok || !tag_is(tokens[i + 1], "CD"))) tk->currency = currency;
            } else if (j > 0 && j < ntks - 1 && seql(tk->text, "2") && isal(tks[j - 1]->text.p[tks[j - 1]->text.n - 1]) && isal(tks[j + 1]->text.p[0])) {
                tk->has_alias = 1; tk->alias = mk("to");
            }
            if (tk->has_alias || !is_none(tk->ph)) { Word *w = &words[nw++]; w->t = aalloc(a, sizeof(Tok *)); w->t[0] = tk; w->n = 1; w->is_list = 0; }
            else if (nw && words[nw - 1].is_list && !words[nw - 1].t[words[nw - 1].n - 1]->ws.n) {
                tk->is_head = 0;
                Word *w = &words[nw - 1]; w->t[w->n++] = tk;
            } else {
                Word *w = &words[nw++]; w->t = aalloc(a, sizeof(Tok *) * (size_t)cap); w->t[0] = tk; w->n = 1;
                w->is_list = tk->ws.n ? 0 : 1;
            }
        }
    }
    for (int i = 0; i < nw; i++) if (words[i].is_list && words[i].n == 1) words[i].is_list = 0;
    return nw;
}

/* ================================================================== G2P */
static TokCtx token_context(TokCtx ctx, S ps, const Tok *tok) {
    int vowel = ctx.future_vowel;
    if (!is_none(ps)) {
        for (int i = 0; i < ps.n; i++) {
            int c = ps.p[i];
            if (is_vowel(c) || is_consonant(c) || is_nonquote_punct(c)) { vowel = is_nonquote_punct(c) ? -1 : is_vowel(c); break; }
        }
    }
    int fto = seql(tok->text, "to") || seql(tok->text, "To") || (seql(tok->text, "TO") && (tag_is(tok, "TO") || tag_is(tok, "IN")));
    return (TokCtx){vowel, fto};
}

static void resolve_tokens(G *g, Tok **tk, int n) {
    arena *a = g->a;
    S text = EMPTY_S;
    for (int i = 0; i < n - 1; i++) text = scat(a, scat(a, text, tk[i]->text), tk[i]->ws);
    text = scat(a, text, tk[n - 1]->text);
    int kinds = 0, seen[3] = {0, 0, 0};
    for (int i = 0; i < text.n; i++) {
        int c = text.p[i]; if (is_subtoken_junk(c)) continue;
        int k = isal(c) ? 0 : isdig(c) ? 1 : 2;
        if (!seen[k]) { seen[k] = 1; kinds++; }
    }
    int prespace = has(text, ' ') || has(text, '/') || kinds > 1;
    for (int i = 0; i < n; i++) {
        if (is_none(tk[i]->ph)) {
            int all_junk = 1; for (int k = 0; k < tk[i]->text.n; k++) if (!is_subtoken_junk(tk[i]->text.p[k])) all_junk = 0;
            if (i == n - 1 && tk[i]->text.n == 1 && is_nonquote_punct(tk[i]->text.p[0])) { tk[i]->ph = tk[i]->text; tk[i]->rating = 3; }
            else if (all_junk) { tk[i]->ph = EMPTY_S; tk[i]->rating = 3; }
        } else if (i > 0) tk[i]->prespace = prespace;
    }
    if (prespace) return;
    typedef struct { int primary, weight, idx; } Ix;
    Ix *ix = aalloc(a, sizeof(Ix) * (size_t)n); int ni = 0;
    for (int i = 0; i < n; i++) if (!is_none(tk[i]->ph) && tk[i]->ph.n) ix[ni++] = (Ix){has(tk[i]->ph, P_PRIMARY), stress_weight(tk[i]->ph), i};
    if (ni == 2 && tk[ix[0].idx]->text.n == 1) { Tok *t = tk[ix[1].idx]; t->ph = apply_stress(g, t->ph, 1, -0.5); return; }
    int sum = 0; for (int i = 0; i < ni; i++) sum += ix[i].primary;
    if (ni < 2 || sum <= (ni + 1) / 2) return;
    for (int i = 0; i < ni; i++) for (int j = i + 1; j < ni; j++) {
        if (ix[j].primary < ix[i].primary || (ix[j].primary == ix[i].primary && (ix[j].weight < ix[i].weight || (ix[j].weight == ix[i].weight && ix[j].idx < ix[i].idx)))) { Ix t = ix[i]; ix[i] = ix[j]; ix[j] = t; }
    }
    for (int i = 0; i < ni / 2; i++) { Tok *t = tk[ix[i].idx]; t->ph = apply_stress(g, t->ph, 1, -0.5); }
}

/* the neural fallback: word runs go through the BART model, punctuation passes through */
static Res fallback(G *g, const Tok *tok) {
    arena *a = g->a;
    S text = tok->text;
    while (text.n && is_space_c(text.p[0])) text = sub(text, 1, text.n);
    while (text.n && is_space_c(text.p[text.n - 1])) text = sub(text, 0, text.n - 1);
    if (!text.n) return R(EMPTY_S, 2);
    unsigned char *out = aalloc(a, (size_t)text.n * 70 + 16); int no = 0;
    int i = 0;
    while (i < text.n) {
        int c = text.p[i];
        if (isal(c)) {
            int j = i; while (j < text.n && isal(text.p[j])) j++;
            for (;;) {   /* (?:['’\-.][^\W\d_]+)* */
                if (j + 1 < text.n && (text.p[j] == '\'' || text.p[j] == S_RSQUOTE || text.p[j] == '-' || text.p[j] == '.') && isal(text.p[j + 1])) {
                    j += 2; while (j < text.n && isal(text.p[j])) j++;
                } else break;
            }
            /* fold: ’ -> ', then strip - . ' from both ends */
            unsigned char *w = aalloc(a, (size_t)(j - i) + 1); int nw = 0;
            for (int k = i; k < j; k++) w[nw++] = text.p[k] == S_RSQUOTE ? '\'' : is_acc(text.p[k]) ? (unsigned char)acc_base(text.p[k]) : text.p[k];
            int lo = 0, hi = nw;
            while (lo < hi && (w[lo] == '-' || w[lo] == '.' || w[lo] == '\'')) lo++;
            while (hi > lo && (w[hi - 1] == '-' || w[hi - 1] == '.' || w[hi - 1] == '\'')) hi--;
            const int limit = g->v->oov.max_pos - 2;
            for (int off = lo; off < hi; off += limit) {
                int len = hi - off < limit ? hi - off : limit;
                int r = tts_oov_word(g->v, w + off, len, out + no, 64);
                if (r < 0) { fail(g, -1, "the fallback model could not phonemize a word"); return R(EMPTY_S, 2); }
                no += r;
            }
            i = j;
        } else if (is_space_c(c)) {
            while (i < text.n && is_space_c(text.p[i])) i++;
            if (no && out[no - 1] != ' ') out[no++] = ' ';
        } else {
            if (is_punct(c)) out[no++] = (unsigned char)c;
            i++;
        }
    }
    int lo = 0; while (lo < no && out[lo] == ' ') lo++;
    while (no > lo && out[no - 1] == ' ') no--;
    out[no] = 0;
    return R(adup(a, out + lo, no - lo), 2);
}

static int g2p_tokens(G *g, S text, Tok ***flat_out) {
    arena *a = g->a;
    Lx lx = {g, &g->v->gold, &g->v->silver};
    while (text.n && is_space_c(text.p[0])) text = sub(text, 1, text.n);     /* text.lstrip() */
    Tok **toks = aalloc(a, sizeof(Tok *) * (size_t)(text.n + 2));
    int nt = tokenize(a, text, toks);
    if (!nt) { *flat_out = toks; return 0; }
    Word *words = aalloc(a, sizeof(Word) * (size_t)(text.n + 8));
    int nw = retokenize(a, toks, nt, words);
    TokCtx ctx = {-1, 0};
    for (int wi = nw - 1; wi >= 0; wi--) {
        Word *w = &words[wi];
        if (!w->is_list) {
            Tok *t = w->t[0];
            if (is_none(t->ph)) { Res r = lexicon_call(&lx, t, &ctx); t->ph = r.ps; t->rating = r.rating; }
            if (is_none(t->ph)) { Res r = fallback(g, t); t->ph = r.ps; t->rating = r.rating; }
            ctx = token_context(ctx, t->ph, t);
            continue;
        }
        int left = 0, right = w->n, should_fallback = 0;
        while (left < right) {
            int blocked = 0;
            for (int k = left; k < right; k++) if (w->t[k]->has_alias || !is_none(w->t[k]->ph)) blocked = 1;
            Tok *merged = NULL; Res r = RES_NONE;
            if (!blocked) { merged = merge_tokens(a, w->t + left, right - left, 0); r = lexicon_call(&lx, merged, &ctx); }
            if (merged && !is_none(r.ps)) {
                w->t[left]->ph = r.ps; w->t[left]->rating = r.rating;
                for (int k = left + 1; k < right; k++) { w->t[k]->ph = EMPTY_S; w->t[k]->rating = r.rating; }
                ctx = token_context(ctx, r.ps, merged);
                right = left; left = 0;
            } else if (left + 1 < right) left++;
            else {
                right--;
                Tok *tk = w->t[right];
                if (is_none(tk->ph)) {
                    int all_junk = 1; for (int k = 0; k < tk->text.n; k++) if (!is_subtoken_junk(tk->text.p[k])) all_junk = 0;
                    if (all_junk) { tk->ph = EMPTY_S; tk->rating = 3; }
                    else { should_fallback = 1; break; }
                }
                left = 0;
            }
        }
        if (should_fallback) {
            Tok *merged = merge_tokens(a, w->t, w->n, 0);
            Res r = fallback(g, merged);
            w->t[0]->ph = r.ps; w->t[0]->rating = r.rating;
            for (int k = 1; k < w->n; k++) { w->t[k]->ph = EMPTY_S; w->t[k]->rating = r.rating; }
        } else resolve_tokens(g, w->t, w->n);
    }
    Tok **flat = aalloc(a, sizeof(Tok *) * (size_t)(nw + 1));
    for (int wi = 0; wi < nw; wi++) flat[wi] = words[wi].is_list ? merge_tokens(a, words[wi].t, words[wi].n, 1) : words[wi].t[0];
    for (int wi = 0; wi < nw; wi++) {
        Tok *t = flat[wi];
        if (!is_none(t->ph) && t->ph.n) {
            unsigned char *d = (unsigned char *)adup(a, t->ph.p, t->ph.n).p;
            for (int k = 0; k < t->ph.n; k++) { if (d[k] == S_TAP) d[k] = 'T'; else if (d[k] == S_GLOTTAL) d[k] = 't'; }
            t->ph = (S){d, t->ph.n};
        }
    }
    *flat_out = flat;
    return nw;
}

/* ================================================================== chunking */
#define MAX_PS_CHARS 510

static S tokens_to_ps(arena *a, Tok **tk, int n) {
    S out = EMPTY_S;
    for (int i = 0; i < n; i++) {
        S ph = is_none(tk[i]->ph) ? EMPTY_S : tk[i]->ph;
        out = scat(a, out, ph);
        if (tk[i]->ws.n) out = scat(a, out, mk(" "));
    }
    int lo = 0, hi = out.n;
    while (lo < hi && out.p[lo] == ' ') lo++;
    while (hi > lo && out.p[hi - 1] == ' ') hi--;
    return sub(out, lo, hi);
}

static int ph_is_one_of(S ph, const char *group_utf8_codes, int ncodes) {
    if (is_none(ph) || ph.n != 1) return 0;
    for (int i = 0; i < ncodes; i++) if (ph.p[0] == (unsigned char)group_utf8_codes[i]) return 1;
    return 0;
}

static int waterfall_last(arena *a, Tok **tk, int n, int next_count) {
    static const unsigned char g1[] = {'!', '.', '?', S_ELLIPSIS}, g2[] = {':', ';'}, g3[] = {',', S_EMDASH};
    const unsigned char *groups[3] = {g1, g2, g3}; const int sizes[3] = {4, 2, 2};
    for (int gi = 0; gi < 3; gi++) {
        int z = -1;
        for (int i = n - 1; i >= 0; i--) if (ph_is_one_of(tk[i]->ph, (const char *)groups[gi], sizes[gi])) { z = i; break; }
        if (z < 0) continue;
        z++;
        if (z < n && ph_is_one_of(tk[z]->ph, (const char[]){')', (char)S_RDQUOTE}, 2)) z++;
        if (next_count - tokens_to_ps(a, tk, z).n <= MAX_PS_CHARS) return z;
    }
    return n;
}

static int en_chunks(arena *a, Tok **tokens, int n, S *chunks) {
    int nc = 0, nheld = 0, pcount = 0;
    Tok **held = aalloc(a, sizeof(Tok *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        Tok *t = tokens[i];
        if (is_none(t->ph)) t->ph = EMPTY_S;
        int next_len = t->ph.n + (t->ws.n ? 1 : 0);
        int rstrip_len = t->ph.n;                                   /* len((ph + " ").rstrip()) */
        while (rstrip_len > 0 && t->ph.p[rstrip_len - 1] == ' ') rstrip_len--;
        int next_pcount = pcount + rstrip_len;
        int lstrip_next = 0;
        if (next_pcount > MAX_PS_CHARS) {
            int z = waterfall_last(a, held, nheld, next_pcount);
            S ps = tokens_to_ps(a, held, z);
            if (ps.n) chunks[nc++] = ps;
            for (int k = z; k < nheld; k++) held[k - z] = held[k];
            nheld -= z;
            pcount = tokens_to_ps(a, held, nheld).n;
            if (!nheld) lstrip_next = 1;
        }
        if (lstrip_next) {                                          /* next_ps.lstrip() */
            int k = 0; while (k < t->ph.n && t->ph.p[k] == ' ') k++;
            next_len = k == t->ph.n ? 0 : next_len - k;
        }
        held[nheld++] = t;
        pcount += next_len;
    }
    if (nheld) { S ps = tokens_to_ps(a, held, nheld); if (ps.n) chunks[nc++] = ps; }
    return nc;
}

/* ================================================================== public front end */
int tts_dict_init(thimble_voice *v, char *err, size_t en) {
    const char *names[2] = {"gold", "silver"};
    tts_dict *d[2] = {&v->gold, &v->silver};
    for (int i = 0; i < 2; i++) {
        char nb[64], ni[64];
        snprintf(nb, sizeof nb, "lex.%s.blob", names[i]); snprintf(ni, sizeof ni, "lex.%s.index", names[i]);
        const tts_tensor *tb = tts_file_find(&v->file, nb), *ti = tts_file_find(&v->file, ni);
        if (!tb || !ti || tb->dtype != TT_U8 || ti->dtype != TT_I32) { snprintf(err, en, "voice file has no %s dictionary", names[i]); return -1; }
        d[i]->blob = (const unsigned char *)tts_file_data(&v->file, tb);
        d[i]->index = (const int32_t *)tts_file_data(&v->file, ti);
        d[i]->n = ti->shape[0];
    }
    return 0;
}

size_t tts_utf8_symbols(const unsigned char *codes, size_t n, char *out, size_t cap) {
    static const char *syms = TTS_SYMBOLS_UTF8;
    size_t k = 0;
    /* split the symbol string once into per-code UTF-8 slices */
    const char *start[TTS_N_SYMBOLS + 1]; int nsym = 0;
    for (const char *p = syms; *p && nsym < TTS_N_SYMBOLS; ) { start[nsym++] = p; p++; while (((unsigned char)*p & 0xC0) == 0x80) p++; start[nsym] = p; }
    for (size_t i = 0; i < n; i++) {
        const char *s; size_t len;
        if (codes[i] < 0x80) { char c = (char)codes[i]; if (k + 1 < cap) out[k] = c; k++; continue; }
        int idx = codes[i] - 0x80;
        if (idx < nsym) { s = start[idx]; len = (size_t)(start[idx + 1] - start[idx]); } else { s = "?"; len = 1; }
        for (size_t j = 0; j < len; j++) { if (k + 1 < cap) out[k] = s[j]; k++; }
    }
    if (cap) out[k < cap ? k : cap - 1] = 0;
    return k;
}

static int run_g2p(const thimble_voice *v, const char *text, arena *a, G *g, S *chunks, int *nchunks) {
    g->v = v; g->a = a; g->fail = 0; g->err[0] = 0;
    S norm = normalize_input(a, text);
    int blank = 1; for (int i = 0; i < norm.n; i++) if (!is_space_c(norm.p[i])) blank = 0;
    if (blank) { fail(g, -2, "text must be a non-empty string"); return g->fail; }
    Tok **flat; int nflat = g2p_tokens(g, norm, &flat);
    if (g->fail) return g->fail;
    *nchunks = en_chunks(a, flat, nflat, chunks);
    if (!*nchunks) { fail(g, -2, "phonemization produced no symbols for this text"); return g->fail; }
    return 0;
}

int tts_phonemize(const thimble_voice *v, const char *text, int *ids, int max_ids, char *err, size_t err_len) {
    arena a = {0}; G g; S *chunks = malloc(sizeof(S) * (strlen(text) + 8));
    int nc = 0, rc = run_g2p(v, text, &a, &g, chunks, &nc), n = 0;
    if (!rc) {
        for (int c = 0; c < nc && !rc; c++) {
            S ch = chunks[c]; if (ch.n > MAX_PS_CHARS) ch.n = MAX_PS_CHARS;
            int start = n;
            if (n + 2 > max_ids) { rc = -3; fail(&g, -3, "phoneme sequence is too long"); break; }
            ids[n++] = 1;
            for (int i = 0; i < ch.n; i++) { int id = TTS_VOCAB_ID[ch.p[i]]; if (id >= 0 && id != 0 && id != 1 && id != 2) { if (n + 2 > max_ids) { rc = -3; fail(&g, -3, "phoneme sequence is too long"); break; } ids[n++] = id; } }
            if (rc) break;
            if (n == start + 1) { rc = -2; fail(&g, -2, "phonemization produced no symbols in the packaged vocabulary"); break; }
            ids[n++] = 2;
        }
        if (!rc && n > 207) { rc = -3; fail(&g, -3, "phoneme sequence has too many tokens (maximum is 207)"); }
    }
    if (rc && err && err_len) snprintf(err, err_len, "%s", g.err);
    afree(&a); free(chunks);
    return rc ? rc : n;
}

int tts_phoneme_string(const thimble_voice *v, const char *text, char *out, size_t cap) {
    arena a = {0}; G g; S *chunks = malloc(sizeof(S) * (strlen(text) + 8));
    int nc = 0, rc = run_g2p(v, text, &a, &g, chunks, &nc);
    size_t k = 0;
    if (!rc) {
        unsigned char *all = malloc(strlen(text) * 70 + 64); size_t na = 0;
        for (int c = 0; c < nc; c++) { memcpy(all + na, chunks[c].p, (size_t)chunks[c].n); na += (size_t)chunks[c].n; }
        k = tts_utf8_symbols(all, na, out, cap);
        free(all);
    } else if (cap) { snprintf(out, cap, "ERROR %d: %s", rc, g.err); k = (size_t)-1; }
    afree(&a); free(chunks);
    return rc ? -1 : (int)k;
}
