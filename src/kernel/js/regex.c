/* Regular expressions for JavaScript: a backtracking matcher over a parsed pattern.
 *
 * Syntax: literals, . [classes] (ranges, negation, \d \w \s inside), \d \D \w \W \s \S,
 * \b \B ^ $, groups ( ) (?: ) (?<name> ), alternation |, lookahead (?= ) (?! ),
 * lookbehind (?<= ) (?<! ), backreferences \1 \k<name>, quantifiers * + ? {n} {n,}
 * {n,m} and their lazy forms, escapes \n \t \r \f \v \0 \xHH \uHHHH \u{H...} \cX.
 * Flags: i (ignore case), m (multiline ^ $), s (dot matches newlines), u, g, y.
 *
 * Text is UTF-8: '.', classes and literals match whole code points. Captures are byte
 * offsets. Matching has a step budget, so a catastrophic pattern fails instead of hanging. */
#include "amdroid/types.h"
#include "string.h"

enum { R_CHAR, R_ANY, R_CLASS, R_BOL, R_EOL, R_WORDB, R_NWORDB, R_GROUP, R_LOOK, R_BACKREF, R_EMPTY };

typedef struct { u32 lo, hi; } rrange_t;
typedef struct rnode {
    int type;
    int min, max;              /* repetition; max -1 = unbounded */
    int lazy;
    u32 cp;                    /* R_CHAR */
    rrange_t *ranges;          /* R_CLASS */
    int nranges, negate;
    int classes;               /* R_CLASS: bits for \d \D \w \W \s \S inside the class */
    int group;                 /* R_GROUP: capture index or -1; R_BACKREF: group */
    int look;                  /* R_LOOK: 0 ahead, 1 negative ahead, 2 behind, 3 negative behind */
    struct rnode **alts;       /* R_GROUP / R_LOOK: the alternatives (sequence heads) */
    int nalts;
    struct rnode *next;
} rnode_t;

typedef struct regex {
    rnode_t *root;             /* group 0 */
    int ngroups;               /* captures, not counting group 0 */
    const char *names[64];     /* named groups by index */
    int icase, multiline, dotall, unicode;
} regex_t;

/* ---- parsing ---- */
typedef struct {
    const char *p, *e;
    void *(*alloc)(u32);
    regex_t *re;
    const char *err;
} rparse_t;

static void *ralloc(rparse_t *ps, u32 n){
    void *m = ps->alloc(n);
    if(m) memset(m, 0, n);
    else ps->err = "out of memory";
    return m;
}
static int hexd(int c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Decodes the UTF-8 code point at *s (advancing it). */
static u32 utf8_next(const unsigned char **s, const unsigned char *e){
    const unsigned char *p = *s;
    u32 c = *p++;
    if(c >= 0xF0 && p + 3 <= e){ c = ((c & 7) << 18) | ((u32)(p[0] & 63) << 12) | ((u32)(p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if(c >= 0xE0 && c < 0xF0 && p + 2 <= e){ c = ((c & 15) << 12) | ((u32)(p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if(c >= 0xC0 && c < 0xE0 && p + 1 <= e){ c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    *s = p;
    return c;
}

static rnode_t *new_node(rparse_t *ps, int type){
    rnode_t *n = ralloc(ps, sizeof(rnode_t));
    if(!n) return NULL;
    n->type = type; n->min = n->max = 1; n->group = -1;
    return n;
}
static int add_range(rparse_t *ps, rnode_t *n, u32 lo, u32 hi){
    rrange_t *r = ralloc(ps, sizeof(rrange_t) * (u32)(n->nranges + 1));
    if(!r) return 0;
    if(n->nranges) memcpy(r, n->ranges, sizeof(rrange_t) * (u32)n->nranges);
    r[n->nranges].lo = lo; r[n->nranges].hi = hi;
    n->ranges = r; n->nranges++;
    return 1;
}

#define CL_D 1
#define CL_ND 2
#define CL_W 4
#define CL_NW 8
#define CL_S 16
#define CL_NS 32

/* An escape after '\' (ps->p at the char after the backslash). Returns the code point,
 * or sets *cls to a class bit for \d \w \s and their negations. in_class: \b is backspace. */
static u32 parse_escape(rparse_t *ps, int *cls, int in_class){
    *cls = 0;
    if(ps->p >= ps->e){ ps->err = "\\ at end of pattern"; return 0; }
    char c = *ps->p++;
    switch(c){
        case 'd': *cls = CL_D; return 0;
        case 'D': *cls = CL_ND; return 0;
        case 'w': *cls = CL_W; return 0;
        case 'W': *cls = CL_NW; return 0;
        case 's': *cls = CL_S; return 0;
        case 'S': *cls = CL_NS; return 0;
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case '0': return 0;
        case 'b': return in_class ? 8 : 'b';
        case 'c':
            if(ps->p < ps->e && ((*ps->p | 32) >= 'a' && (*ps->p | 32) <= 'z')) return (u32)(*ps->p++ & 31);
            return 'c';
        case 'x':
            if(ps->p + 1 < ps->e && hexd(ps->p[0]) >= 0 && hexd(ps->p[1]) >= 0){
                u32 v = (u32)(hexd(ps->p[0]) * 16 + hexd(ps->p[1])); ps->p += 2; return v;
            }
            return 'x';
        case 'u': {
            if(ps->p < ps->e && *ps->p == '{'){
                const char *q = ps->p + 1; u32 v = 0;
                while(q < ps->e && hexd(*q) >= 0){ v = v * 16 + (u32)hexd(*q); q++; }
                if(q < ps->e && *q == '}'){ ps->p = q + 1; return v; }
                return 'u';
            }
            if(ps->p + 3 < ps->e && hexd(ps->p[0]) >= 0 && hexd(ps->p[1]) >= 0 && hexd(ps->p[2]) >= 0 && hexd(ps->p[3]) >= 0){
                u32 v = (u32)((hexd(ps->p[0]) << 12) | (hexd(ps->p[1]) << 8) | (hexd(ps->p[2]) << 4) | hexd(ps->p[3]));
                ps->p += 4;
                /* a surrogate pair written as two escapes */
                if(v >= 0xD800 && v < 0xDC00 && ps->p + 5 < ps->e && ps->p[0] == '\\' && ps->p[1] == 'u'){
                    const char *q = ps->p + 2; u32 lo = 0; int ok = 1;
                    for(int k = 0; k < 4; k++){ int h = hexd(q[k]); if(h < 0){ ok = 0; break; } lo = lo * 16 + (u32)h; }
                    if(ok && lo >= 0xDC00 && lo < 0xE000){ v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); ps->p += 6; }
                }
                return v;
            }
            return 'u';
        }
    }
    /* any other char stands for itself (including UTF-8 lead bytes) */
    if((unsigned char)c >= 0x80){
        const unsigned char *q = (const unsigned char *)ps->p - 1;
        u32 v = utf8_next(&q, (const unsigned char *)ps->e);
        ps->p = (const char *)q;
        return v;
    }
    return (u32)(unsigned char)c;
}

static u32 parse_literal_cp(rparse_t *ps){
    const unsigned char *q = (const unsigned char *)ps->p;
    u32 v = utf8_next(&q, (const unsigned char *)ps->e);
    ps->p = (const char *)q;
    return v;
}

static rnode_t *parse_class(rparse_t *ps){
    rnode_t *n = new_node(ps, R_CLASS);
    if(!n) return NULL;
    if(ps->p < ps->e && *ps->p == '^'){ n->negate = 1; ps->p++; }
    int first = 1;
    while(ps->p < ps->e && (*ps->p != ']' || (first && 0))){
        first = 0;
        u32 lo; int cls = 0;
        if(*ps->p == '\\'){ ps->p++; lo = parse_escape(ps, &cls, 1); }
        else lo = parse_literal_cp(ps);
        if(cls){ n->classes |= cls; continue; }
        u32 hi = lo;
        if(ps->p + 1 < ps->e && *ps->p == '-' && ps->p[1] != ']'){
            ps->p++;
            int c2 = 0;
            if(*ps->p == '\\'){ ps->p++; hi = parse_escape(ps, &c2, 1); }
            else hi = parse_literal_cp(ps);
            if(c2){ add_range(ps, n, lo, lo); add_range(ps, n, '-', '-'); n->classes |= c2; continue; }
            if(hi < lo){ ps->err = "range out of order in character class"; return NULL; }
        }
        if(!add_range(ps, n, lo, hi)) return NULL;
    }
    if(ps->p >= ps->e){ ps->err = "unterminated character class"; return NULL; }
    ps->p++;   /* ] */
    return n;
}

static rnode_t *parse_alts(rparse_t *ps, rnode_t *group);

/* {n}, {n,}, {n,m}: returns 1 and moves past it, or 0 (then '{' is a literal) */
static int parse_braces(rparse_t *ps, int *mn, int *mx){
    const char *q = ps->p + 1;
    if(q >= ps->e || *q < '0' || *q > '9') return 0;
    int a = 0; while(q < ps->e && *q >= '0' && *q <= '9'){ a = a * 10 + (*q - '0'); if(a > 100000) a = 100000; q++; }
    int b = a;
    if(q < ps->e && *q == ','){
        q++;
        if(q < ps->e && *q >= '0' && *q <= '9'){ b = 0; while(q < ps->e && *q >= '0' && *q <= '9'){ b = b * 10 + (*q - '0'); if(b > 100000) b = 100000; q++; } }
        else b = -1;
    }
    if(q >= ps->e || *q != '}') return 0;
    ps->p = q + 1;
    *mn = a; *mx = b;
    return 1;
}

/* One sequence (until | or ) or the end). */
static rnode_t *parse_seq(rparse_t *ps){
    rnode_t *head = NULL, *tail = NULL;
    while(ps->p < ps->e && *ps->p != '|' && *ps->p != ')' && !ps->err){
        rnode_t *n = NULL;
        char c = *ps->p;
        if(c == '^'){ ps->p++; n = new_node(ps, R_BOL); }
        else if(c == '$'){ ps->p++; n = new_node(ps, R_EOL); }
        else if(c == '.'){ ps->p++; n = new_node(ps, R_ANY); }
        else if(c == '['){ ps->p++; n = parse_class(ps); }
        else if(c == '('){
            ps->p++;
            n = new_node(ps, R_GROUP);
            if(!n) return NULL;
            if(ps->p + 1 < ps->e && ps->p[0] == '?'){
                char k = ps->p[1];
                if(k == ':'){ ps->p += 2; }
                else if(k == '=' || k == '!'){ n->type = R_LOOK; n->look = k == '=' ? 0 : 1; ps->p += 2; }
                else if(k == '<' && ps->p + 2 < ps->e && (ps->p[2] == '=' || ps->p[2] == '!')){
                    n->type = R_LOOK; n->look = ps->p[2] == '=' ? 2 : 3; ps->p += 3;
                } else if(k == '<'){
                    const char *q = ps->p + 2, *s0 = q;
                    while(q < ps->e && *q != '>') q++;
                    if(q >= ps->e){ ps->err = "invalid capture group name"; return NULL; }
                    n->group = ++ps->re->ngroups;
                    if(n->group < 64){
                        char *nm = ralloc(ps, (u32)(q - s0) + 1);
                        if(!nm) return NULL;
                        memcpy(nm, s0, (u32)(q - s0)); nm[q - s0] = 0;
                        ps->re->names[n->group] = nm;
                    }
                    ps->p = q + 1;
                } else { ps->err = "invalid group"; return NULL; }
            } else n->group = ++ps->re->ngroups;
            parse_alts(ps, n);
            if(ps->err) return NULL;
            if(ps->p >= ps->e || *ps->p != ')'){ ps->err = "unterminated group"; return NULL; }
            ps->p++;
        }
        else if(c == '\\'){
            ps->p++;
            if(ps->p < ps->e && (*ps->p == 'b' || *ps->p == 'B')){ n = new_node(ps, *ps->p == 'b' ? R_WORDB : R_NWORDB); ps->p++; }
            else if(ps->p < ps->e && *ps->p >= '1' && *ps->p <= '9'){
                int g = 0; while(ps->p < ps->e && *ps->p >= '0' && *ps->p <= '9'){ g = g * 10 + (*ps->p - '0'); ps->p++; }
                n = new_node(ps, R_BACKREF); if(n) n->group = g;
            } else if(ps->p + 1 < ps->e && *ps->p == 'k' && ps->p[1] == '<'){
                const char *q = ps->p + 2, *s0 = q;
                while(q < ps->e && *q != '>') q++;
                n = new_node(ps, R_BACKREF);
                if(n){
                    n->group = -1;
                    for(int g = 1; g <= ps->re->ngroups && g < 64; g++){
                        const char *nm = ps->re->names[g];
                        if(nm && strlen(nm) == (u32)(q - s0) && memcmp(nm, s0, (u32)(q - s0)) == 0) n->group = g;
                    }
                    if(n->group < 0) n->group = 99999;   /* resolved later names are rare: no match */
                }
                ps->p = q < ps->e ? q + 1 : q;
            } else {
                int cls; u32 v = parse_escape(ps, &cls, 0);
                if(cls){ n = new_node(ps, R_CLASS); if(n) n->classes = cls; }
                else { n = new_node(ps, R_CHAR); if(n) n->cp = v; }
            }
        }
        else if(c == '*' || c == '+' || c == '?'){ ps->err = "nothing to repeat"; return NULL; }
        else if(c == '{'){
            int a, b;
            const char *save = ps->p;
            if(parse_braces(ps, &a, &b)){ ps->p = save; ps->err = "nothing to repeat"; return NULL; }
            ps->p++; n = new_node(ps, R_CHAR); if(n) n->cp = '{';
        }
        else { n = new_node(ps, R_CHAR); if(n) n->cp = parse_literal_cp(ps); }
        if(!n || ps->err) return NULL;
        /* a quantifier */
        if(ps->p < ps->e){
            char q = *ps->p; int a = -2, b = -2;
            if(q == '*'){ a = 0; b = -1; ps->p++; }
            else if(q == '+'){ a = 1; b = -1; ps->p++; }
            else if(q == '?'){ a = 0; b = 1; ps->p++; }
            else if(q == '{'){ if(!parse_braces(ps, &a, &b)) a = -2; }
            if(a != -2){
                if(n->type == R_BOL || n->type == R_EOL || n->type == R_WORDB || n->type == R_NWORDB){ ps->err = "nothing to repeat"; return NULL; }
                if(b != -1 && b < a){ ps->err = "numbers out of order in {} quantifier"; return NULL; }
                n->min = a; n->max = b;
                if(ps->p < ps->e && *ps->p == '?'){ n->lazy = 1; ps->p++; }
            }
        }
        if(tail) tail->next = n; else head = n;
        tail = n;
    }
    if(!head) head = new_node(ps, R_EMPTY);
    return head;
}

static rnode_t *parse_alts(rparse_t *ps, rnode_t *group){
    rnode_t *alts[256]; int n = 0;
    for(;;){
        rnode_t *s = parse_seq(ps);
        if(ps->err) return NULL;
        if(n < 256) alts[n++] = s;
        if(ps->p < ps->e && *ps->p == '|'){ ps->p++; continue; }
        break;
    }
    group->alts = ralloc(ps, sizeof(rnode_t *) * (u32)n);
    if(!group->alts) return NULL;
    memcpy(group->alts, alts, sizeof(rnode_t *) * (u32)n);
    group->nalts = n;
    return group;
}

/* Compiles a pattern. Returns NULL and an error message on a syntax error. */
regex_t *re_compile(const char *pat, u32 plen, const char *flags, void *(*alloc)(u32), const char **err){
    rparse_t ps;
    ps.p = pat; ps.e = pat + plen; ps.alloc = alloc; ps.err = NULL;
    ps.re = alloc(sizeof(regex_t));
    if(!ps.re){ *err = "out of memory"; return NULL; }
    memset(ps.re, 0, sizeof(regex_t));
    for(const char *f = flags; f && *f; f++){
        if(*f == 'i') ps.re->icase = 1;
        else if(*f == 'm') ps.re->multiline = 1;
        else if(*f == 's') ps.re->dotall = 1;
        else if(*f == 'u') ps.re->unicode = 1;
    }
    rnode_t *root = new_node(&ps, R_GROUP);
    if(!root){ *err = ps.err; return NULL; }
    root->group = 0;
    parse_alts(&ps, root);
    if(!ps.err && ps.p < ps.e) ps.err = *ps.p == ')' ? "unmatched ')'" : "unexpected character";
    if(ps.err){ *err = ps.err; return NULL; }
    ps.re->root = root;
    *err = NULL;
    return ps.re;
}
int re_ngroups(const regex_t *re){ return re ? re->ngroups : 0; }
const char *re_group_name(const regex_t *re, int i){ return re && i > 0 && i < 64 ? re->names[i] : NULL; }

/* ---- matching ---- */
typedef struct kf {
    int (*fn)(const struct kf *self, u32 pos);
    const struct kf *up;
    const rnode_t *node;
    int count;
    u32 start;
} kf_t;

typedef struct {
    const regex_t *re;
    const unsigned char *t;
    u32 len;
    int *caps;
    long steps;
    int depth;
    int aborted;
    u32 end;           /* where the whole match ended */
    u32 look_target;   /* lookbehind: the position the inner match must end at */
} mstate_t;

static mstate_t *M;    /* the match in progress (matching does not nest) */

#define STEP_BUDGET 20000000L
#define DEPTH_LIMIT 12000

static int run(const rnode_t *n, u32 pos, const kf_t *k);

static int lower_cp(u32 c){
    if(c >= 'A' && c <= 'Z') return (int)(c + 32);
    if(c >= 0xC0 && c <= 0xDE && c != 0xD7) return (int)(c + 32);
    if(c >= 0x410 && c <= 0x42F) return (int)(c + 32);      /* Cyrillic */
    return (int)c;
}
static int is_word_cp(u32 c){ return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_space_cp(u32 c){
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
static int class_has(const rnode_t *n, u32 c){
    int in = 0;
    int cl = n->classes;
    if(cl){
        if((cl & CL_D) && c >= '0' && c <= '9') in = 1;
        else if((cl & CL_ND) && !(c >= '0' && c <= '9')) in = 1;
        else if((cl & CL_W) && is_word_cp(c)) in = 1;
        else if((cl & CL_NW) && !is_word_cp(c)) in = 1;
        else if((cl & CL_S) && is_space_cp(c)) in = 1;
        else if((cl & CL_NS) && !is_space_cp(c)) in = 1;
    }
    for(int i = 0; i < n->nranges && !in; i++){
        if(c >= n->ranges[i].lo && c <= n->ranges[i].hi) in = 1;
        else if(M->re->icase){
            u32 lc = (u32)lower_cp(c);
            u32 uc = (c >= 'a' && c <= 'z') ? c - 32 : (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? c - 32 : c;
            if((lc >= n->ranges[i].lo && lc <= n->ranges[i].hi) || (uc >= n->ranges[i].lo && uc <= n->ranges[i].hi)) in = 1;
        }
    }
    return n->negate ? !in : in;
}
/* Does a single-character atom match at pos? Sets *np past it. */
static int one_char(const rnode_t *n, u32 pos, u32 *np){
    if(pos >= M->len) return 0;
    const unsigned char *s = M->t + pos;
    u32 c = utf8_next(&s, M->t + M->len);
    switch(n->type){
        case R_CHAR:
            if(c != n->cp && !(M->re->icase && lower_cp(c) == lower_cp(n->cp))) return 0;
            break;
        case R_ANY:
            if(!M->re->dotall && (c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029)) return 0;
            break;
        case R_CLASS:
            if(!class_has(n, c)) return 0;
            break;
        default: return 0;
    }
    *np = (u32)(s - M->t);
    return 1;
}
static u32 prev_pos(u32 pos){
    if(pos == 0) return 0;
    pos--;
    while(pos > 0 && (M->t[pos] & 0xC0) == 0x80) pos--;
    return pos;
}
static int at_word(u32 pos){ return pos < M->len && is_word_cp(M->t[pos]); }

static int k_seq(const kf_t *k, u32 pos){ return run(k->node->next, pos, k->up); }

/* group end: record the capture, then continue; undo it when that fails */
static int k_group_end(const kf_t *k, u32 pos){
    int g = k->node->group;
    int *caps = M->caps;
    int os = caps[2 * g], oe = caps[2 * g + 1];
    caps[2 * g] = (int)k->start; caps[2 * g + 1] = (int)pos;
    if(k->up->fn(k->up, pos)) return 1;
    caps[2 * g] = os; caps[2 * g + 1] = oe;
    return 0;
}
static int k_accept(const kf_t *k, u32 pos){ (void)k; M->end = pos; return 1; }
static int k_look_end(const kf_t *k, u32 pos){ (void)k; return pos == M->look_target; }

/* Matches node n once, then calls after. */
static int atom(const rnode_t *n, u32 pos, const kf_t *after){
    if(++M->steps > STEP_BUDGET || M->depth > DEPTH_LIMIT){ M->aborted = 1; return 0; }
    u32 np;
    switch(n->type){
        case R_CHAR: case R_ANY: case R_CLASS:
            if(!one_char(n, pos, &np)) return 0;
            return after->fn(after, np);
        case R_EMPTY: return after->fn(after, pos);
        case R_BOL:
            if(pos == 0 || (M->re->multiline && (M->t[pos-1] == '\n' || M->t[pos-1] == '\r'))) return after->fn(after, pos);
            return 0;
        case R_EOL:
            if(pos == M->len || (M->re->multiline && (M->t[pos] == '\n' || M->t[pos] == '\r'))) return after->fn(after, pos);
            return 0;
        case R_WORDB: case R_NWORDB: {
            int a = pos > 0 && is_word_cp(M->t[pos-1]), b = at_word(pos);
            if((a != b) == (n->type == R_WORDB)) return after->fn(after, pos);
            return 0;
        }
        case R_BACKREF: {
            int g = n->group;
            if(g > M->re->ngroups) return 0;
            int s = M->caps[2 * g], e = M->caps[2 * g + 1];
            if(s < 0 || e < s) return after->fn(after, pos);          /* an unset group matches empty */
            u32 L = (u32)(e - s);
            if(pos + L > M->len) return 0;
            for(u32 i = 0; i < L; i++){
                int a = M->t[s + i], b = M->t[pos + i];
                if(a != b && !(M->re->icase && lower_cp((u32)a) == lower_cp((u32)b))) return 0;
            }
            return after->fn(after, pos + L);
        }
        case R_GROUP: {
            kf_t ge = { k_group_end, after, n, 0, pos };
            const kf_t *k = n->group >= 0 ? &ge : after;
            M->depth++;
            for(int i = 0; i < n->nalts; i++){
                if(run(n->alts[i], pos, k)){ M->depth--; return 1; }
                if(M->aborted) break;
            }
            M->depth--;
            return 0;
        }
        case R_LOOK: {
            int found = 0;
            int *saved = NULL;
            int ncap = 2 * (M->re->ngroups + 1);
            int saved_buf[64];
            if(ncap <= 64){ saved = saved_buf; memcpy(saved, M->caps, sizeof(int) * (u32)ncap); }
            u32 save_end = M->end, save_target = M->look_target;
            M->depth++;
            if(n->look <= 1){
                kf_t acc = { k_accept, NULL, n, 0, 0 };
                for(int i = 0; i < n->nalts && !found; i++) found = run(n->alts[i], pos, &acc);
            } else {
                kf_t le = { k_look_end, NULL, n, 0, 0 };
                M->look_target = pos;
                for(u32 st = pos; ; st = prev_pos(st)){
                    for(int i = 0; i < n->nalts && !found; i++) found = run(n->alts[i], st, &le);
                    if(found || st == 0 || M->aborted) break;
                }
            }
            M->depth--;
            M->end = save_end; M->look_target = save_target;
            int negative = n->look == 1 || n->look == 3;
            if(negative){
                if(saved) memcpy(M->caps, saved, sizeof(int) * (u32)ncap);
                return found ? 0 : after->fn(after, pos);
            }
            if(!found){ if(saved) memcpy(M->caps, saved, sizeof(int) * (u32)ncap); return 0; }
            if(after->fn(after, pos)) return 1;
            if(saved) memcpy(M->caps, saved, sizeof(int) * (u32)ncap);
            return 0;
        }
    }
    return 0;
}

/* repetition of a complex atom (a group): one more iteration, or stop */
static int k_rep(const kf_t *k, u32 pos);
static int rep(const rnode_t *n, u32 pos, int count, u32 last, const kf_t *k){
    if(M->aborted) return 0;
    int can_more = n->max < 0 || count < n->max;
    int can_stop = count >= n->min;
    /* an iteration that matched nothing ends the loop (avoids looping forever) */
    if(count > n->min && pos == last) can_more = 0;
    kf_t again = { k_rep, k, n, count + 1, pos };
    M->depth++;
    int r = 0;
    if(n->lazy){
        if(can_stop && run(n->next, pos, k)) r = 1;
        else if(can_more && !M->aborted) r = atom(n, pos, &again);
    } else {
        if(can_more && atom(n, pos, &again)) r = 1;
        else if(can_stop && !M->aborted) r = run(n->next, pos, k);
    }
    M->depth--;
    return r;
}
static int k_rep(const kf_t *k, u32 pos){ return rep(k->node, pos, k->count, k->start, k->up); }

static int run(const rnode_t *n, u32 pos, const kf_t *k){
    if(!n) return k->fn(k, pos);
    if(M->aborted) return 0;
    if(n->min == 1 && n->max == 1){
        kf_t nx = { k_seq, k, n, 0, 0 };
        return atom(n, pos, &nx);
    }
    if(n->type == R_CHAR || n->type == R_ANY || n->type == R_CLASS){
        /* a single-character atom: scan iteratively, then backtrack one character at a time */
        if(++M->steps > STEP_BUDGET){ M->aborted = 1; return 0; }
        int count = 0; u32 p = pos, np;
        if(n->lazy){
            while(count < n->min){ if(!one_char(n, p, &np)) return 0; p = np; count++; }
            for(;;){
                if(run(n->next, p, k)) return 1;
                if(M->aborted) return 0;
                if(n->max >= 0 && count >= n->max) return 0;
                if(!one_char(n, p, &np)) return 0;
                p = np; count++;
                if(++M->steps > STEP_BUDGET){ M->aborted = 1; return 0; }
            }
        }
        while((n->max < 0 || count < n->max) && one_char(n, p, &np)){ p = np; count++; }
        if(count < n->min) return 0;
        for(;;){
            if(run(n->next, p, k)) return 1;
            if(M->aborted || count == n->min) return 0;
            p = prev_pos(p); count--;
            if(++M->steps > STEP_BUDGET){ M->aborted = 1; return 0; }
        }
    }
    return rep(n, pos, 0, (u32)-1, k);
}

/* Matches re against text starting at start (sticky: only there). caps receives
 * 2 * (ngroups + 1) byte offsets (-1 for groups that did not take part).
 * Returns 1 on a match, 0 on none, -1 when the step budget ran out. */
int re_exec(const regex_t *re, const char *text, u32 len, u32 start, int sticky, int *caps){
    mstate_t ms;
    mstate_t *saved = M;
    M = &ms;
    ms.re = re; ms.t = (const unsigned char *)text; ms.len = len; ms.caps = caps;
    ms.steps = 0; ms.depth = 0; ms.aborted = 0; ms.end = 0; ms.look_target = 0;
    int ncap = 2 * (re->ngroups + 1);
    kf_t acc = { k_accept, NULL, NULL, 0, 0 };
    int result = 0;
    for(u32 s = start; s <= len; ){
        for(int i = 0; i < ncap; i++) caps[i] = -1;
        if(atom(re->root, s, &acc)){ caps[0] = (int)s; caps[1] = (int)ms.end; result = 1; break; }
        if(ms.aborted){ result = -1; break; }
        if(sticky || s == len) break;
        /* the next code point (a match never starts inside a UTF-8 sequence) */
        s++;
        while(s < len && (ms.t[s] & 0xC0) == 0x80) s++;
    }
    M = saved;
    return result;
}

/* Compatibility: find the first match of pat in text from start. */
static char re_scratch[1 << 16];
static u32 re_scratch_used;
static void *scratch_alloc(u32 n){
    n = (n + 7) & ~7u;
    if(re_scratch_used + n > sizeof re_scratch) return NULL;
    void *p = re_scratch + re_scratch_used;
    re_scratch_used += n;
    return p;
}
int re_search(const char *pat, u32 plen, int icase, const char *text, u32 tlen, u32 start, u32 *ms, u32 *me){
    re_scratch_used = 0;
    const char *err;
    regex_t *re = re_compile(pat, plen, icase ? "i" : "", scratch_alloc, &err);
    if(!re) return 0;
    int caps[128];
    if(re->ngroups >= 63) return 0;
    if(re_exec(re, text, tlen, start, 0, caps) != 1) return 0;
    *ms = (u32)caps[0]; *me = (u32)caps[1];
    return 1;
}
