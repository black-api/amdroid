/* A backtracking regular-expression matcher for JavaScript regex syntax:
 * literals, ., [classes] (ranges, ^ negation), \d \w \s and their negations,
 * groups (...), alternation |, anchors ^ $ and \b, and the quantifiers * + ? {m} {m,}
 * {m,n} (lazy with a following ?). Groups are matched but not returned. Flag i ignores case. */
#include "amdroid/types.h"
#include "string.h"

typedef int (*rx_k)(const char *s, void *kc);
typedef struct { const char *base, *end; int icase; } rx_cx_t;

static int lc(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int uc(int c){ return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int is_digit(int c){ return c >= '0' && c <= '9'; }
static int is_word(int c){ return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_space(int c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* The position after the atom that starts at p. */
static const char *atom_end(const char *p, const char *pe){
    if(*p == '\\') return p + 2 <= pe ? p + 2 : pe;
    if(*p == '['){
        const char *q = p + 1;
        if(q < pe && *q == '^') q++;
        if(q < pe && *q == ']') q++;
        while(q < pe && *q != ']'){
            if(*q == '\\' && q + 1 < pe) q++;
            q++;
        }
        return q < pe ? q + 1 : pe;
    }
    if(*p == '('){
        int depth = 0;
        for(const char *q = p; q < pe; q++){
            if(*q == '\\' && q + 1 < pe){ q++; continue; }
            if(*q == '['){ q = atom_end(q, pe) - 1; continue; }
            if(*q == '(') depth++;
            if(*q == ')'){ depth--; if(depth == 0) return q + 1; }
        }
        return pe;
    }
    return p + 1;
}

/* Does the class atom [a, ae) match character c? */
static int class_match(const char *a, const char *ae, int c, int icase){
    const char *q = a + 1, *end = ae - 1;      /* the text between [ and ] */
    int neg = 0, hit = 0;
    if(q < end && *q == '^'){ neg = 1; q++; }
    while(q < end){
        if(*q == '\\' && q + 1 < end){
            char e = q[1];
            q += 2;
            if(e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S'){
                int m = e == 'd' ? is_digit(c) : e == 'D' ? !is_digit(c) :
                        e == 'w' ? is_word(c) : e == 'W' ? !is_word(c) :
                        e == 's' ? is_space(c) : !is_space(c);
                if(m) hit = 1;
                continue;
            }
            int v = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            if(c == v || (icase && lc(c) == lc(v))) hit = 1;
            continue;
        }
        int lo = (unsigned char)*q++;
        int hi = lo;
        if(q + 1 < end && *q == '-'){ q++; hi = (unsigned char)*q++; }
        if((c >= lo && c <= hi) || (icase && ((lc(c) >= lc(lo) && lc(c) <= lc(hi)) || (uc(c) >= uc(lo) && uc(c) <= uc(hi)))))
            hit = 1;
    }
    return neg ? !hit : hit;
}

/* Does the single-character atom a..ae match character c? */
static int single(const char *a, const char *ae, int c, int icase){
    if(*a == '.') return c != '\n';
    if(*a == '[') return class_match(a, ae, c, icase);
    if(*a == '\\'){
        char e = a[1];
        if(e == 'd') return is_digit(c);
        if(e == 'D') return !is_digit(c);
        if(e == 'w') return is_word(c);
        if(e == 'W') return !is_word(c);
        if(e == 's') return is_space(c);
        if(e == 'S') return !is_space(c);
        if(e == 'n') return c == '\n';
        if(e == 't') return c == '\t';
        return icase ? lc(c) == lc(e) : c == e;
    }
    return icase ? lc(c) == lc(*a) : c == *a;
}

static int match_alt(rx_cx_t *cx, const char *p, const char *pe, const char *s, rx_k k, void *kc);
static int match_seq(rx_cx_t *cx, const char *p, const char *pe, const char *s, rx_k k, void *kc);

/* A repetition in progress. */
typedef struct rep {
    rx_cx_t *cx;
    const char *a, *ae, *pn, *pe;
    int min, max, lazy, count;
    const char *at;          /* where this round started (empty-loop guard) */
    rx_k k; void *kc;
} rep_t;

static int rep_step(rep_t *r, const char *s);

static int rep_after_atom(const char *s, void *kc){
    rep_t *r = (rep_t *)kc;
    if(s == r->at && r->count >= r->min) return 0;      /* an empty round cannot repeat forever */
    rep_t next = *r;
    next.count = r->count + 1;
    return rep_step(&next, s);
}

/* Tries the atom once more at s, then what follows (greedy), or what follows, then one more atom (lazy). */
static int rep_step(rep_t *r, const char *s){
    r->at = s;
    int can_more = r->count < r->max;
    if(!r->lazy){
        if(can_more){
            if(r->a[0] == '('){
                if(match_alt(r->cx, r->a + 1, r->ae - 1, s, rep_after_atom, r)) return 1;
            } else if(s < r->cx->end && single(r->a, r->ae, (unsigned char)*s, r->cx->icase)){
                rep_t next = *r;
                next.count = r->count + 1;
                if(rep_step(&next, s + 1)) return 1;
            }
        }
        if(r->count >= r->min) return match_seq(r->cx, r->pn, r->pe, s, r->k, r->kc);
        return 0;
    }
    if(r->count >= r->min && match_seq(r->cx, r->pn, r->pe, s, r->k, r->kc)) return 1;
    if(!can_more) return 0;
    if(r->a[0] == '('){
        return match_alt(r->cx, r->a + 1, r->ae - 1, s, rep_after_atom, r);
    }
    if(s < r->cx->end && single(r->a, r->ae, (unsigned char)*s, r->cx->icase)){
        rep_t next = *r;
        next.count = r->count + 1;
        return rep_step(&next, s + 1);
    }
    return 0;
}

/* Reads the quantifier after an atom. Sets min, max and lazy; returns the next position. */
static const char *quant(const char *q, const char *pe, int *min, int *max, int *lazy){
    *min = 1; *max = 1; *lazy = 0;
    if(q >= pe) return q;
    if(*q == '*'){ *min = 0; *max = 1 << 20; q++; }
    else if(*q == '+'){ *min = 1; *max = 1 << 20; q++; }
    else if(*q == '?'){ *min = 0; *max = 1; q++; }
    else if(*q == '{'){
        const char *r = q + 1;
        int a = 0, b;
        while(r < pe && is_digit(*r)){ a = a * 10 + (*r - '0'); r++; }
        if(r < pe && *r == ','){
            r++;
            b = 0;
            if(r < pe && is_digit(*r)){ while(r < pe && is_digit(*r)){ b = b * 10 + (*r - '0'); r++; } }
            else b = 1 << 20;
        } else b = a;
        if(!(r < pe && *r == '}')) return q;
        *min = a; *max = b;
        q = r + 1;
    } else return q;
    if(q < pe && *q == '?'){ *lazy = 1; q++; }
    return q;
}

static int match_seq(rx_cx_t *cx, const char *p, const char *pe, const char *s, rx_k k, void *kc){
    if(p >= pe) return k(s, kc);
    if(*p == '^'){
        if(s != cx->base) return 0;
        return match_seq(cx, p + 1, pe, s, k, kc);
    }
    if(*p == '$'){
        if(s != cx->end) return 0;
        return match_seq(cx, p + 1, pe, s, k, kc);
    }
    if(*p == '\\' && p + 1 < pe && p[1] == 'b'){
        int before = s > cx->base && is_word((unsigned char)s[-1]);
        int after = s < cx->end && is_word((unsigned char)*s);
        if(before == after) return 0;
        return match_seq(cx, p + 2, pe, s, k, kc);
    }
    const char *ae = atom_end(p, pe);
    int min, max, lazy;
    const char *pn = quant(ae, pe, &min, &max, &lazy);
    rep_t r;
    r.cx = cx; r.a = p; r.ae = ae; r.pn = pn; r.pe = pe;
    r.min = min; r.max = max; r.lazy = lazy; r.count = 0;
    r.at = NULL;
    r.k = k; r.kc = kc;
    return rep_step(&r, s);
}

/* Matches p..pe at s, where p..pe may hold top-level alternatives separated by '|'. */
static int match_alt(rx_cx_t *cx, const char *p, const char *pe, const char *s, rx_k k, void *kc){
    int depth = 0;
    const char *start = p;
    for(const char *q = p; q <= pe; q++){
        if(q == pe || (*q == '|' && depth == 0)){
            if(match_seq(cx, start, q, s, k, kc)) return 1;
            start = q + 1;
            continue;
        }
        if(*q == '\\' && q + 1 < pe){ q++; continue; }
        if(*q == '['){ q = atom_end(q, pe) - 1; continue; }
        if(*q == '(') depth++;
        if(*q == ')') depth--;
    }
    return 0;
}

static int accept(const char *s, void *kc){
    *(const char **)kc = s;
    return 1;
}

/* Searches text from start. On a match returns 1, with the range in *ms and *me. */
int re_search(const char *pat, u32 plen, int icase, const char *text, u32 tlen, u32 start, u32 *ms, u32 *me){
    rx_cx_t cx;
    cx.base = text;
    cx.end = text + tlen;
    cx.icase = icase;
    for(u32 st = start; st <= tlen; st++){
        const char *endp = NULL;
        if(match_alt(&cx, pat, pat + plen, text + st, accept, &endp)){
            *ms = st;
            *me = (u32)(endp - text);
            return 1;
        }
    }
    return 0;
}
