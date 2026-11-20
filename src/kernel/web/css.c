/* CSS: the cascade for a page.
 *
 * Selectors: type, #id, .class (any number), [attr], [attr=v] and ~= |= ^= $= *= (with i),
 * the descendant, child (>), next-sibling (+) and sibling (~) combinators, :first/last/only-child,
 * :first/last/only-of-type, :nth-(last-)child/of-type(an+b), :empty, :root, :link, :checked,
 * :disabled, :enabled, :placeholder-shown, :not(list), :is/:where(list); states that never hold
 * here (:hover, :focus...) and pseudo-elements never match.
 * At-rules: @media (width ranges, colour scheme, hover, resolution...; nested), @supports, @layer.
 * Values: custom properties and var() with fallbacks, !important, calc/min/max/clamp, px em rem
 * % vw vh pt ch, colours with alpha (blended over what is behind), hsl().
 *
 * Sheets are parsed once per change (web_css_gen), declarations are parsed with the rule, and
 * rules are indexed by the key of their rightmost compound, so styling an element only looks at
 * rules that could match it. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "string.h"

#define SEL_MAX 8              /* compounds in one selector */
#define DPR_NUM WEB_DPR_NUM
#define DPR_DEN WEB_DPR_DEN

/* ================= arena ================= */

typedef struct achunk { struct achunk *next; u32 used, cap; } achunk_t;
typedef struct { achunk_t *head; } arena_t;

static void *ar_alloc(arena_t *a, u32 n){
    n = (n + 7) & ~7u;
    if(!a->head || a->head->used + n > a->head->cap){
        u32 cap = n > 32768 ? n : 65536 - (u32)sizeof(achunk_t);
        achunk_t *c = kmalloc(sizeof(achunk_t) + cap);
        if(!c) return NULL;
        c->next = a->head; c->used = 0; c->cap = cap;
        a->head = c;
    }
    void *p = (char *)(a->head + 1) + a->head->used;
    a->head->used += n;
    return p;
}
static void ar_free(arena_t *a){
    while(a->head){ achunk_t *nx = a->head->next; kfree(a->head); a->head = nx; }
}
static char *ar_str(arena_t *a, const char *s, u32 n){
    char *d = ar_alloc(a, n + 1);
    if(!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static arena_t rarena;          /* rules: selectors, declarations, copies of their text */
static arena_t qarena;          /* css_matches: the selector being asked about */
static arena_t parena;          /* strings styles point to during one styling pass (grid areas) */
static arena_t *cur_arena = &rarena;

/* ================= small helpers ================= */

static int c_lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int c_ws(int c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int c_digit(int c){ return c >= '0' && c <= '9'; }
static int hexv(int c){
    if(c >= '0' && c <= '9') return c - '0';
    c = c_lower(c);
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static void trim(const char **s, u32 *n){
    while(*n && c_ws((*s)[0])){ (*s)++; (*n)--; }
    while(*n && c_ws((*s)[*n - 1])) (*n)--;
}
static int eq_n(const char *a, u32 n, const char *lit){
    u32 k = 0;
    while(k < n && lit[k] && a[k] == lit[k]) k++;
    return k == n && lit[k] == 0;
}
static int starts_n(const char *a, u32 n, const char *lit){
    u32 k = 0;
    while(lit[k]){ if(k >= n || a[k] != lit[k]) return 0; k++; }
    return 1;
}
static u32 hash_n(const char *s, u32 n){
    u32 h = 2166136261u;
    for(u32 i = 0; i < n; i++){ h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* One bit per name: an element's signature has the bits of its tag, id and classes. */
static u64 name_bit(const char *s, u32 n){ return (u64)1 << (hash_n(s, n) & 63); }
static u64 str_bit(const char *s){ return name_bit(s, (u32)strlen(s)); }
static u64 node_sig(const wnode_t *n){
    u64 m = str_bit(n->tag);
    if(n->id) m |= str_bit(n->id);
    if(n->cls){
        for(const char *p = n->cls; *p; ){
            while(c_ws(*p)) p++;
            const char *b = p;
            while(*p && !c_ws(*p)) p++;
            if(p > b) m |= name_bit(b, (u32)(p - b));
        }
    }
    return m;
}
static int use_sig = 0;          /* 1 while styling the tree top-down: signatures and sibling numbers are fresh */

/* ================= selectors ================= */

enum { P_FIRST = 1, P_LAST = 2, P_EMPTY = 4, P_FIRST_T = 8, P_LAST_T = 16, P_CHECKED = 32,
       P_DISABLED = 64, P_ENABLED = 128, P_LINK = 256, P_PH_SHOWN = 512, P_ROOT = 1024, P_ACTIVE_IN = 2048, P_FOCUS = 4096 };

typedef struct { const char *name, *val; u8 op, icase; } asel_t;    /* op: 0 present, = ~ | ^ $ * */
typedef struct { u8 kind; short a, b; } nth_t;                      /* 1 child, 2 last-child, 3 of-type, 4 last-of-type */
struct sel;
typedef struct compound {
    const char *tag, *id;
    const char **cls; u8 ncls;
    asel_t *attr; u8 nattr;
    nth_t nth[2]; u8 nnth;
    u16 pseudo;
    u8 never;
    char comb;                     /* how it relates to the compound on its left: ' ', '>', '+', '~' */
    struct sel *nots; u8 nnots;    /* :not(a, b): none of them may match */
    struct sel *is[4]; u8 nis[4];  /* :is(a, b) :where(c): one of each list must match */
    u8 nisl;
    u64 need;                      /* signature bits an element must have to match (a quick test) */
} compound_t;
typedef struct sel { compound_t *c; u8 n; int spec; } sel_t;

static int sel_stop(char ch){
    return ch == '#' || ch == '.' || ch == ':' || ch == '[' || c_ws(ch) || ch == '>' || ch == '+' ||
           ch == '~' || ch == ')' || ch == '(' || ch == ',' || ch == '|' || ch == ']' || ch == '=' ||
           ch == '"' || ch == '\'' || ch == '*' || ch == '^' || ch == '$' || ch == '!';
}

/* A name in a selector, with CSS escapes undone (Tailwind: .md\:flex, .w-1\/2). */
static const char *read_name(const char *s, u32 n, u32 *i, int lower_it, u32 maxlen){
    char buf[160]; u32 k = 0;
    while(*i < n){
        char c = s[*i];
        if(c == '\\' && *i + 1 < n){
            (*i)++;
            if(hexv(s[*i]) >= 0){
                u32 cp = 0, d = 0;
                while(*i < n && d < 6 && hexv(s[*i]) >= 0){ cp = cp * 16 + (u32)hexv(s[*i]); (*i)++; d++; }
                if(*i < n && c_ws(s[*i])) (*i)++;
                c = cp < 128 ? (char)cp : '?';
            } else { c = s[*i]; (*i)++; }
        } else if(sel_stop(c) || c == '{' || c == '}' || c == ';') break;
        else (*i)++;
        if(k + 1 < sizeof buf) buf[k++] = lower_it ? (char)c_lower(c) : c;
    }
    if(k > maxlen) k = maxlen;
    return ar_str(cur_arena, buf, k);
}

static int parse_selector(const char *s, u32 n, sel_t *out);

/* A selector list ("a, b.c > d") into an array; the count, or 0. */
static int parse_list(const char *s, u32 n, sel_t **out){
    sel_t tmp[16]; int k = 0;
    u32 start = 0; int depth = 0; char q = 0;
    for(u32 i = 0; i <= n; i++){
        if(i < n){
            char c = s[i];
            if(q){ if(c == q) q = 0; continue; }
            if(c == '"' || c == '\'') { q = c; continue; }
            if(c == '(' || c == '[') depth++;
            if(c == ')' || c == ']') depth--;
            if(c != ',' || depth > 0) continue;
        }
        const char *sp = s + start; u32 sn = i - start;
        trim(&sp, &sn);
        if(sn && k < 16 && parse_selector(sp, sn, &tmp[k]) == 0) k++;
        start = i + 1;
    }
    if(!k) return 0;
    *out = ar_alloc(cur_arena, sizeof(sel_t) * (u32)k);
    if(!*out) return 0;
    memcpy(*out, tmp, sizeof(sel_t) * (u32)k);
    return k;
}

/* "2n+1", "odd", "-n+3", "4" (an " of S" part is ignored). 0 if not understood. */
static int parse_nth(const char *s, u32 n, short *a, short *b){
    trim(&s, &n);
    char v[32]; u32 k = 0;
    for(u32 i = 0; i < n && k + 1 < sizeof v; i++){ if(s[i] == ' ' && i + 3 < n && s[i+1] == 'o' && s[i+2] == 'f') break; if(!c_ws(s[i])) v[k++] = (char)c_lower(s[i]); }
    v[k] = 0;
    if(strcmp(v, "odd") == 0){ *a = 2; *b = 1; return 1; }
    if(strcmp(v, "even") == 0){ *a = 2; *b = 0; return 1; }
    u32 i = 0; int sign = 1, num = 0, any = 0;
    if(v[i] == '-' || v[i] == '+'){ sign = v[i] == '-' ? -1 : 1; i++; }
    while(c_digit(v[i])){ num = num * 10 + (v[i] - '0'); i++; any = 1; }
    if(v[i] == 'n'){
        *a = (short)(sign * (any ? num : 1));
        i++;
        int bs = 1, bn = 0;
        if(v[i] == '+' || v[i] == '-'){ bs = v[i] == '-' ? -1 : 1; i++; while(c_digit(v[i])){ bn = bn * 10 + (v[i] - '0'); i++; } }
        *b = (short)(bs * bn);
        return v[i] == 0;
    }
    if(!any || v[i]) return 0;
    *a = 0; *b = (short)(sign * num);
    return 1;
}

/* Parses one selector ("div.note > p#x") into compounds. 0 if it is usable. */
static int parse_selector(const char *s, u32 n, sel_t *r){
    compound_t parts[SEL_MAX];
    int np = 0, spec = 0;
    u32 i = 0;
    char comb = ' ';
    while(i < n){
        while(i < n && c_ws(s[i])) i++;
        if(i >= n) break;
        if(s[i] == '>' || s[i] == '+' || s[i] == '~'){ comb = s[i]; i++; continue; }
        if(np >= SEL_MAX) return -1;
        compound_t *c = &parts[np];
        memset(c, 0, sizeof *c);
        c->comb = np ? comb : ' ';
        comb = ' ';
        const char *cls[12]; int ncls = 0;
        asel_t attr[6]; int nattr = 0;
        sel_t nots[8]; int nnots = 0;
        while(i < n && !c_ws(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~'){
            char ch = s[i];
            if(ch == '#'){
                i++;
                c->id = read_name(s, n, &i, 0, 120);
                spec += 100;
            } else if(ch == '.'){
                i++;
                const char *nm = read_name(s, n, &i, 0, 150);
                if(nm && ncls < 12) cls[ncls++] = nm;
                spec += 10;
            } else if(ch == '['){
                i++;
                while(i < n && c_ws(s[i])) i++;
                asel_t a; memset(&a, 0, sizeof a);
                a.name = read_name(s, n, &i, 1, 60);
                while(i < n && c_ws(s[i])) i++;
                if(i < n && s[i] == '|' && !(i + 1 < n && s[i+1] == '=')){ c->never = 1; }   /* namespaces */
                if(i < n && s[i] != ']'){
                    if(s[i] == '=') { a.op = '='; i++; }
                    else if(i + 1 < n && s[i+1] == '='){ a.op = (u8)s[i]; i += 2; }
                    else c->never = 1;
                    while(i < n && c_ws(s[i])) i++;
                    if(i < n && (s[i] == '"' || s[i] == '\'')){
                        char q = s[i++]; u32 b = i;
                        while(i < n && s[i] != q) i++;
                        a.val = ar_str(cur_arena, s + b, i - b);
                        if(i < n) i++;
                    } else {
                        a.val = read_name(s, n, &i, 0, 150);
                    }
                    while(i < n && c_ws(s[i])) i++;
                    if(i < n && (s[i] == 'i' || s[i] == 'I')){ a.icase = 1; i++; }
                    else if(i < n && (s[i] == 's' || s[i] == 'S')) i++;
                }
                while(i < n && s[i] != ']') i++;
                if(i < n) i++;
                if(nattr < 6 && a.name) attr[nattr++] = a;
                spec += 10;
            } else if(ch == ':'){
                i++;
                int element = i < n && s[i] == ':';
                if(element) i++;
                u32 b = i;
                while(i < n && !sel_stop(s[i])) i++;
                u32 pl = i - b;
                const char *pn = s + b;
                u32 as = 0, ae = 0;                        /* the (...) argument */
                if(i < n && s[i] == '('){
                    int depth = 0;
                    as = i + 1;
                    while(i < n){ if(s[i] == '(') depth++; if(s[i] == ')'){ depth--; if(depth == 0) break; } i++; }
                    ae = i;
                    if(i < n) i++;
                }
                #define PIS(lit) eq_n(pn, pl, lit)
                if(element || PIS("before") || PIS("after") || PIS("first-line") || PIS("first-letter")){
                    c->never = 1;                          /* no boxes for pseudo-elements */
                    spec += 1;
                    continue;
                }
                spec += 10;
                if(PIS("first-child")) c->pseudo |= P_FIRST;
                else if(PIS("last-child")) c->pseudo |= P_LAST;
                else if(PIS("only-child")) c->pseudo |= P_FIRST | P_LAST;
                else if(PIS("first-of-type")) c->pseudo |= P_FIRST_T;
                else if(PIS("last-of-type")) c->pseudo |= P_LAST_T;
                else if(PIS("only-of-type")) c->pseudo |= P_FIRST_T | P_LAST_T;
                else if(PIS("empty")) c->pseudo |= P_EMPTY;
                else if(PIS("root") || PIS("scope")) c->pseudo |= P_ROOT;
                else if(PIS("link") || PIS("any-link") || PIS("-webkit-any-link")) c->pseudo |= P_LINK;
                else if(PIS("checked")) c->pseudo |= P_CHECKED;
                else if(PIS("disabled")) c->pseudo |= P_DISABLED;
                else if(PIS("enabled")) c->pseudo |= P_ENABLED;
                else if(PIS("placeholder-shown")) c->pseudo |= P_PH_SHOWN;
                /* there is no hovering pointer here: a clicked-open element counts as hovered */
                else if(PIS("hover") || PIS("focus-within")) c->pseudo |= P_ACTIVE_IN;
                else if(PIS("focus") || PIS("focus-visible")) c->pseudo |= P_FOCUS;
                else if(PIS("defined") || PIS("lang") || PIS("dir") || PIS("optional") || PIS("read-write") ||
                        PIS("valid") || PIS("in-range")) { }                /* always true here */
                else if((PIS("nth-child") || PIS("nth-last-child") || PIS("nth-of-type") || PIS("nth-last-of-type")) && ae > as){
                    short a, bb;
                    if(c->nnth < 2 && parse_nth(s + as, ae - as, &a, &bb)){
                        u8 kind = PIS("nth-child") ? 1 : PIS("nth-last-child") ? 2 : PIS("nth-of-type") ? 3 : 4;
                        c->nth[c->nnth++] = (nth_t){ kind, a, bb };
                    } else c->never = 1;
                }
                else if(PIS("not") && ae > as){
                    sel_t *l; int k = parse_list(s + as, ae - as, &l);
                    for(int q = 0; q < k && nnots < 8; q++) nots[nnots++] = l[q];
                }
                else if((PIS("is") || PIS("where") || PIS("matches") || PIS("-webkit-any") || PIS("-moz-any")) && ae > as){
                    sel_t *l; int k = parse_list(s + as, ae - as, &l);
                    if(k && c->nisl < 4){ c->is[c->nisl] = l; c->nis[c->nisl] = (u8)k; c->nisl++; }
                    else if(!k) c->never = 1;
                    if(PIS("where")) spec -= 10;
                }
                else c->never = 1;                         /* :hover, :focus, :target, :has()...: never here */
                #undef PIS
            } else if(ch == '*'){
                i++;
            } else if(ch == '|' || ch == '(' || ch == ')' || ch == ',' || ch == '!' || ch == '"' || ch == '\''){
                c->never = 1;
                i++;
            } else {
                c->tag = read_name(s, n, &i, 1, sizeof ((wnode_t *)0)->tag - 1);
                if(c->tag && !c->tag[0]){ c->tag = NULL; i++; c->never = 1; }   /* a stray character */
                spec += 1;
            }
        }
        if(ncls){
            c->cls = ar_alloc(cur_arena, sizeof(char *) * (u32)ncls);
            if(c->cls){ memcpy(c->cls, cls, sizeof(char *) * (u32)ncls); c->ncls = (u8)ncls; }
        }
        if(nattr){
            c->attr = ar_alloc(cur_arena, sizeof(asel_t) * (u32)nattr);
            if(c->attr){ memcpy(c->attr, attr, sizeof(asel_t) * (u32)nattr); c->nattr = (u8)nattr; }
        }
        if(nnots){
            c->nots = ar_alloc(cur_arena, sizeof(sel_t) * (u32)nnots);
            if(c->nots){ memcpy(c->nots, nots, sizeof(sel_t) * (u32)nnots); c->nnots = (u8)nnots; }
        }
        c->need = (c->tag ? str_bit(c->tag) : 0) | (c->id ? str_bit(c->id) : 0);
        for(int k = 0; k < c->ncls; k++) c->need |= str_bit(c->cls[k]);
        np++;
    }
    if(np == 0) return -1;
    r->c = ar_alloc(cur_arena, sizeof(compound_t) * (u32)np);
    if(!r->c) return -1;
    memcpy(r->c, parts, sizeof(compound_t) * (u32)np);
    r->n = (u8)np;
    r->spec = spec < 0 ? 0 : spec;
    return 0;
}

/* ================= matching ================= */

static int has_class(const char *list, const char *cls){
    if(!list || !cls[0]) return 0;
    const char *p = list;
    while(*p){
        while(*p && c_ws(*p)) p++;
        const char *b = p;
        while(*p && !c_ws(*p)) p++;
        u32 n = (u32)(p - b);
        u32 k = 0;
        while(k < n && cls[k] && b[k] == cls[k]) k++;
        if(k == n && cls[k] == 0) return 1;
    }
    return 0;
}

static const char *node_attr(const wnode_t *n, const char *name){
    switch(name[0]){
    case 'i': if(strcmp(name, "id") == 0) return n->id; break;
    case 'c': if(strcmp(name, "class") == 0) return n->cls; break;
    case 'h': if(strcmp(name, "href") == 0) return n->href;
              if(strcmp(name, "hidden") == 0) return n->hidden ? "" : NULL; break;
    case 's': if(strcmp(name, "style") == 0) return n->style; break;
    case 'v': if(strcmp(name, "value") == 0) return n->value; break;
    case 'o': if(strcmp(name, "onclick") == 0) return n->onclick; break;
    case 'a': if(strcmp(name, "alt") == 0) return strcmp(n->tag, "img") == 0 ? n->alt : NULL; break;
    case 'p': if(strcmp(name, "placeholder") == 0) return strcmp(n->tag, "img") != 0 ? n->alt : NULL; break;
    }
    return wnode_attr_get((wnode_t *)n, name);
}

static int ci_eq(const char *a, const char *b, u32 n, int icase){
    for(u32 i = 0; i < n; i++){
        char x = a[i], y = b[i];
        if(icase){ x = (char)c_lower(x); y = (char)c_lower(y); }
        if(x != y) return 0;
    }
    return 1;
}
static int attr_match(const wnode_t *n, const asel_t *a){
    const char *v = node_attr(n, a->name);
    if(!v) return 0;
    if(!a->op) return 1;
    const char *w = a->val ? a->val : "";
    u32 vl = (u32)strlen(v), wl = (u32)strlen(w);
    switch(a->op){
    case '=': return vl == wl && ci_eq(v, w, vl, a->icase);
    case '^': return wl && vl >= wl && ci_eq(v, w, wl, a->icase);
    case '$': return wl && vl >= wl && ci_eq(v + vl - wl, w, wl, a->icase);
    case '*':
        if(!wl) return 0;
        for(u32 i = 0; i + wl <= vl; i++) if(ci_eq(v + i, w, wl, a->icase)) return 1;
        return 0;
    case '|': return (vl == wl && ci_eq(v, w, vl, a->icase)) || (vl > wl && v[wl] == '-' && ci_eq(v, w, wl, a->icase));
    case '~':
        if(!wl) return 0;
        for(u32 i = 0; i < vl; ){
            while(i < vl && c_ws(v[i])) i++;
            u32 b = i;
            while(i < vl && !c_ws(v[i])) i++;
            if(i - b == wl && ci_eq(v + b, w, wl, a->icase)) return 1;
        }
        return 0;
    }
    return 0;
}

static const wnode_t *prev_elem(const wnode_t *n){
    if(!n->parent) return NULL;
    const wnode_t *prev = NULL;
    for(const wnode_t *c = n->parent->first; c && c != n; c = c->next) if(!c->text) prev = c;
    return prev;
}

/* Numbers the element children of p: position and count, overall and per tag. */
void css_number_children(wnode_t *p){
    u16 i = 0;
    for(wnode_t *c = p->first; c; c = c->next) if(!c->text) c->eidx = ++i;
    for(wnode_t *c = p->first; c; c = c->next){
        if(c->text) continue;
        c->ecnt = i;
        c->tidx = 0;
    }
    for(wnode_t *c = p->first; c; c = c->next){
        if(c->text || c->tidx) continue;
        u16 k = 0;
        for(wnode_t *d = c; d; d = d->next) if(!d->text && strcmp(d->tag, c->tag) == 0) d->tidx = ++k;
        for(wnode_t *d = c; d; d = d->next) if(!d->text && strcmp(d->tag, c->tag) == 0) d->tcnt = k;
    }
}

/* Position (1-based) of n among its element siblings: kind 1 from the start, 2 from the end,
 * 3 and 4 the same among siblings of its type. */
static int sib_pos(const wnode_t *n, int kind){
    if(use_sig && n->eidx){
        if(kind == 1) return n->eidx;
        if(kind == 2) return n->ecnt - n->eidx + 1;
        if(kind == 3) return n->tidx;
        return n->tcnt - n->tidx + 1;
    }
    if(!n->parent) return 1;
    int before = 0, after = 0, seen = 0;
    for(const wnode_t *c = n->parent->first; c; c = c->next){
        if(c->text) continue;
        if(c == n){ seen = 1; continue; }
        if(kind >= 3 && strcmp(c->tag, n->tag) != 0) continue;
        if(seen) after++; else before++;
    }
    return (kind == 1 || kind == 3) ? before + 1 : after + 1;
}
static int nth_ok(int pos, short a, short b){
    if(a == 0) return pos == b;
    int d = pos - b;
    if(d % a) return 0;
    return d / a >= 0;
}

static int sel_match(const wnode_t *n, const sel_t *r);

static int form_tag(const char *t){
    return strcmp(t, "input") == 0 || strcmp(t, "button") == 0 || strcmp(t, "select") == 0 ||
           strcmp(t, "textarea") == 0 || strcmp(t, "option") == 0 || strcmp(t, "fieldset") == 0;
}

static int compound_match(const wnode_t *n, const compound_t *c){
    if(n->text || c->never) return 0;
    if(use_sig && (n->sig & c->need) != c->need) return 0;
    if(c->tag && strcmp(c->tag, n->tag) != 0) return 0;
    if(c->id && (!n->id || strcmp(c->id, n->id) != 0)) return 0;
    for(int k = 0; k < c->ncls; k++) if(!has_class(n->cls, c->cls[k])) return 0;
    for(int k = 0; k < c->nattr; k++) if(!attr_match(n, &c->attr[k])) return 0;
    if(c->pseudo){
        u16 p = c->pseudo;
        if((p & P_FIRST) && sib_pos(n, 1) != 1) return 0;
        if((p & P_LAST) && sib_pos(n, 2) != 1) return 0;
        if((p & P_FIRST_T) && sib_pos(n, 3) != 1) return 0;
        if((p & P_LAST_T) && sib_pos(n, 4) != 1) return 0;
        if(p & P_EMPTY){
            for(const wnode_t *ch = n->first; ch; ch = ch->next) if(!ch->text || ch->tlen) return 0;
        }
        if((p & P_ROOT) && strcmp(n->tag, "html") != 0) return 0;
        if((p & P_LINK) && !((strcmp(n->tag, "a") == 0 || strcmp(n->tag, "area") == 0) && n->href)) return 0;
        if((p & P_CHECKED) && !wnode_attr_get((wnode_t *)n, "checked") && !wnode_attr_get((wnode_t *)n, "selected")) return 0;
        if((p & P_DISABLED) && !(form_tag(n->tag) && wnode_attr_get((wnode_t *)n, "disabled"))) return 0;
        if((p & P_ENABLED) && !(form_tag(n->tag) && !wnode_attr_get((wnode_t *)n, "disabled"))) return 0;
        if((p & P_PH_SHOWN) && !(n->alt && (!n->value || !n->value[0]) && strcmp(n->tag, "img") != 0)) return 0;
        if(p & P_ACTIVE_IN){
            const wnode_t *a = web_active;
            while(a && a != n) a = a->parent;
            if(!a) return 0;
        }
        if((p & P_FOCUS) && n != web_active) return 0;
    }
    for(int k = 0; k < c->nnth; k++)
        if(!nth_ok(sib_pos(n, c->nth[k].kind), c->nth[k].a, c->nth[k].b)) return 0;
    for(int k = 0; k < c->nnots; k++) if(sel_match(n, &c->nots[k])) return 0;
    for(int L = 0; L < c->nisl; L++){
        int any = 0;
        for(int k = 0; k < c->nis[L] && !any; k++) any = sel_match(n, &c->is[L][k]);
        if(!any) return 0;
    }
    return 1;
}

/* Does n match compound k and, through the combinators, everything to its left? */
static int match_from(const wnode_t *n, const sel_t *r, int k){
    if(!compound_match(n, &r->c[k])) return 0;
    if(k == 0) return 1;
    char comb = r->c[k].comb;
    if(comb == '>') return n->parent && match_from(n->parent, r, k - 1);
    if(comb == '+'){ const wnode_t *p = prev_elem(n); return p && match_from(p, r, k - 1); }
    if(comb == '~'){
        for(const wnode_t *p = prev_elem(n); p; p = prev_elem(p)) if(match_from(p, r, k - 1)) return 1;
        return 0;
    }
    for(const wnode_t *a = n->parent; a; a = a->parent) if(match_from(a, r, k - 1)) return 1;
    return 0;
}
static int sel_match(const wnode_t *n, const sel_t *r){
    return r->n > 0 && match_from(n, r, r->n - 1);
}

/* Does element n match the selector list sel ("div.a, #b p")? For querySelector: the last
 * list asked about is kept parsed, so a querySelectorAll over the page parses it once. */
int css_matches(const wnode_t *n, const char *sel){
    static char last[256];
    static sel_t *list;
    static int nlist = -1;
    u32 len = (u32)strlen(sel);
    if(nlist < 0 || len >= sizeof last || strcmp(last, sel) != 0){
        ar_free(&qarena);
        cur_arena = &qarena;
        nlist = parse_list(sel, len, &list);
        cur_arena = &rarena;
        if(len < sizeof last) memcpy(last, sel, len + 1); else last[0] = 0;
    }
    int saved = use_sig;
    use_sig = 0;
    int m = 0;
    for(int i = 0; i < nlist && !m; i++) m = sel_match(n, &list[i]);
    use_sig = saved;
    if(len >= sizeof last) nlist = -1;
    return m;
}

/* ================= declarations ================= */

enum {
    PR_NONE, PR_CUSTOM, PR_COLOR, PR_BG, PR_BGCOLOR, PR_BGIMAGE, PR_FONT, PR_FONTSIZE, PR_FONTWEIGHT,
    PR_FONTSTYLE, PR_WHITESPACE, PR_TEXTDEC, PR_TEXTALIGN, PR_TEXTTRANSFORM, PR_TEXTINDENT, PR_DISPLAY,
    PR_WIDTH, PR_MINWIDTH, PR_MAXWIDTH, PR_HEIGHT, PR_MINHEIGHT, PR_MAXHEIGHT, PR_LISTSTYLE,
    PR_FLEX, PR_FLEXGROW, PR_FLEXSHRINK, PR_FLEXBASIS, PR_FLEXWRAP, PR_FLEXDIR, PR_FLEXFLOW, PR_JUSTIFY,
    PR_BOXSIZING, PR_OVERFLOW, PR_VISIBILITY,
    PR_BORDER, PR_BORDER_SIDE, PR_BORDER_SIDE_R, PR_BORDER_SIDE_B, PR_BORDER_SIDE_L,
    PR_BORDER_W, PR_BORDER_W_SIDE, PR_BORDER_W_R, PR_BORDER_W_B, PR_BORDER_W_L,
    PR_BORDER_C, PR_BORDER_C_SIDE, PR_BORDER_STYLE, PR_BORDER_S_SIDE, PR_BORDER_S_R, PR_BORDER_S_B, PR_BORDER_S_L,
    PR_GAP, PR_ROWGAP, PR_COLGAP, PR_GRIDCOLS, PR_GRIDTEMPLATE, PR_GRIDCOLUMN, PR_GRIDAREA,
    PR_MARGIN, PR_MARGIN_T, PR_MARGIN_R, PR_MARGIN_B, PR_MARGIN_L,
    PR_PADDING, PR_PADDING_T, PR_PADDING_R, PR_PADDING_B, PR_PADDING_L,
    PR_MARGIN_INLINE, PR_MARGIN_BLOCK, PR_PADDING_INLINE, PR_PADDING_BLOCK,
    PR_POSITION, PR_FLOAT, PR_OPACITY, PR_CLIP, PR_CLIPPATH, PR_TOP, PR_RIGHT, PR_BOTTOM, PR_LEFT, PR_INSET,
    PR_TRANSFORM, PR_CONTENT_VIS, PR_MASK, PR_GRIDROW, PR_GRIDAREAS, PR_GRIDCOLSTART, PR_GRIDCOLEND
};
static const struct { const char *name; u8 id; } props[] = {
    { "color", PR_COLOR }, { "background", PR_BG }, { "background-color", PR_BGCOLOR },
    { "background-image", PR_BGIMAGE }, { "font", PR_FONT }, { "font-size", PR_FONTSIZE },
    { "font-weight", PR_FONTWEIGHT }, { "font-style", PR_FONTSTYLE }, { "white-space", PR_WHITESPACE },
    { "text-decoration", PR_TEXTDEC }, { "text-decoration-line", PR_TEXTDEC }, { "text-align", PR_TEXTALIGN },
    { "text-transform", PR_TEXTTRANSFORM }, { "text-indent", PR_TEXTINDENT }, { "display", PR_DISPLAY },
    { "width", PR_WIDTH }, { "min-width", PR_MINWIDTH }, { "max-width", PR_MAXWIDTH },
    { "inline-size", PR_WIDTH }, { "max-inline-size", PR_MAXWIDTH },
    { "height", PR_HEIGHT }, { "min-height", PR_MINHEIGHT }, { "max-height", PR_MAXHEIGHT },
    { "block-size", PR_HEIGHT }, { "list-style", PR_LISTSTYLE }, { "list-style-type", PR_LISTSTYLE },
    { "flex", PR_FLEX }, { "flex-grow", PR_FLEXGROW }, { "flex-shrink", PR_FLEXSHRINK }, { "flex-basis", PR_FLEXBASIS },
    { "flex-wrap", PR_FLEXWRAP }, { "flex-direction", PR_FLEXDIR }, { "flex-flow", PR_FLEXFLOW },
    { "justify-content", PR_JUSTIFY }, { "box-sizing", PR_BOXSIZING }, { "overflow", PR_OVERFLOW },
    { "overflow-y", PR_OVERFLOW }, { "visibility", PR_VISIBILITY },
    { "border", PR_BORDER }, { "border-top", PR_BORDER_SIDE }, { "border-right", PR_BORDER_SIDE_R },
    { "border-bottom", PR_BORDER_SIDE_B }, { "border-left", PR_BORDER_SIDE_L },
    { "border-block-start", PR_BORDER_SIDE }, { "border-block-end", PR_BORDER_SIDE_B },
    { "border-inline-start", PR_BORDER_SIDE_L }, { "border-inline-end", PR_BORDER_SIDE_R },
    { "border-width", PR_BORDER_W }, { "border-top-width", PR_BORDER_W_SIDE }, { "border-right-width", PR_BORDER_W_R },
    { "border-bottom-width", PR_BORDER_W_B }, { "border-left-width", PR_BORDER_W_L },
    { "border-color", PR_BORDER_C }, { "border-top-color", PR_BORDER_C_SIDE }, { "border-right-color", PR_BORDER_C_SIDE },
    { "border-bottom-color", PR_BORDER_C_SIDE }, { "border-left-color", PR_BORDER_C_SIDE },
    { "border-style", PR_BORDER_STYLE }, { "border-top-style", PR_BORDER_S_SIDE }, { "border-right-style", PR_BORDER_S_R },
    { "border-bottom-style", PR_BORDER_S_B }, { "border-left-style", PR_BORDER_S_L },
    { "gap", PR_GAP }, { "grid-gap", PR_GAP }, { "row-gap", PR_ROWGAP }, { "grid-row-gap", PR_ROWGAP },
    { "column-gap", PR_COLGAP }, { "grid-column-gap", PR_COLGAP },
    { "grid-template-columns", PR_GRIDCOLS }, { "grid-template", PR_GRIDTEMPLATE }, { "grid", PR_GRIDTEMPLATE },
    { "grid-column", PR_GRIDCOLUMN }, { "grid-area", PR_GRIDAREA }, { "grid-row", PR_GRIDROW },
    { "grid-template-areas", PR_GRIDAREAS }, { "grid-column-start", PR_GRIDCOLSTART }, { "grid-column-end", PR_GRIDCOLEND },
    { "margin", PR_MARGIN }, { "margin-top", PR_MARGIN_T }, { "margin-right", PR_MARGIN_R },
    { "margin-bottom", PR_MARGIN_B }, { "margin-left", PR_MARGIN_L },
    { "margin-block-start", PR_MARGIN_T }, { "margin-block-end", PR_MARGIN_B },
    { "margin-inline-start", PR_MARGIN_L }, { "margin-inline-end", PR_MARGIN_R },
    { "padding", PR_PADDING }, { "padding-top", PR_PADDING_T }, { "padding-right", PR_PADDING_R },
    { "padding-bottom", PR_PADDING_B }, { "padding-left", PR_PADDING_L },
    { "padding-block-start", PR_PADDING_T }, { "padding-block-end", PR_PADDING_B },
    { "padding-inline-start", PR_PADDING_L }, { "padding-inline-end", PR_PADDING_R },
    { "margin-inline", PR_MARGIN_INLINE }, { "margin-block", PR_MARGIN_BLOCK },
    { "padding-inline", PR_PADDING_INLINE }, { "padding-block", PR_PADDING_BLOCK },
    { "position", PR_POSITION }, { "float", PR_FLOAT }, { "opacity", PR_OPACITY }, { "clip", PR_CLIP },
    { "clip-path", PR_CLIPPATH }, { "left", PR_LEFT }, { "top", PR_TOP }, { "right", PR_RIGHT }, { "bottom", PR_BOTTOM },
    { "inset", PR_INSET }, { "inset-inline-start", PR_LEFT }, { "inset-inline-end", PR_RIGHT },
    { "inset-block-start", PR_TOP }, { "inset-block-end", PR_BOTTOM }, { "transform", PR_TRANSFORM },
    { "content-visibility", PR_CONTENT_VIS }, { "mask", PR_MASK }, { "mask-image", PR_MASK },
};
static u8 prop_id(const char *s, u32 n){
    char b[40];
    if(n >= sizeof b) return PR_NONE;
    if(n > 2 && s[0] == '-' && s[1] == '-') return PR_CUSTOM;
    for(u32 i = 0; i < n; i++) b[i] = (char)c_lower(s[i]);
    b[n] = 0;
    const char *p = b;
    if(n > 8 && memcmp(b, "-webkit-", 8) == 0) p += 8;          /* -webkit-flex and friends */
    for(u32 i = 0; i < sizeof props / sizeof props[0]; i++) if(strcmp(p, props[i].name) == 0) return props[i].id;
    return PR_NONE;
}

typedef struct { u8 id, imp, var; const char *name; u16 nl; const char *v; u32 vl; } decl_t;

/* Splits "a: b; c: d !important" into declarations (pointing into text). Returns how many. */
static int split_decls(const char *d, u32 n, decl_t *out, int max){
    int k = 0;
    u32 i = 0;
    while(i < n && k < max){
        u32 ds = i;
        int depth = 0; char q = 0;
        while(i < n){
            char c = d[i];
            if(q){ if(c == '\\') i++; else if(c == q) q = 0; i++; continue; }
            if(c == '"' || c == '\''){ q = c; i++; continue; }
            if(c == '(') depth++;
            else if(c == ')') depth--;
            else if(c == ';' && depth <= 0) break;
            i++;
        }
        u32 de = i;
        if(i < n) i++;
        u32 colon = ds;
        while(colon < de && d[colon] != ':') colon++;
        if(colon >= de) continue;
        const char *pn = d + ds; u32 pl = colon - ds;
        trim(&pn, &pl);
        u8 id = prop_id(pn, pl);
        if(id == PR_NONE) continue;
        const char *v = d + colon + 1; u32 vl = de - colon - 1;
        trim(&v, &vl);
        u8 imp = 0;
        if(vl >= 10){
            /* "!important", maybe with spaces after the '!' */
            u32 e = vl;
            while(e && c_ws(v[e - 1])) e--;
            if(e >= 9 && eq_n(v + e - 9, 9, "important") ){
                u32 b = e - 9;
                while(b && c_ws(v[b - 1])) b--;
                if(b && v[b - 1] == '!'){ imp = 1; vl = b - 1; while(vl && c_ws(v[vl - 1])) vl--; }
            }
        }
        u8 var = 0;
        for(u32 j = 0; j + 4 <= vl; j++) if(v[j] == 'v' && v[j+1] == 'a' && v[j+2] == 'r' && v[j+3] == '('){ var = 1; break; }
        out[k++] = (decl_t){ id, imp, var, pn, (u16)pl, v, vl };
    }
    return k;
}

/* ================= rules, @media ================= */

typedef struct { sel_t s; int order; u16 media; decl_t *d; u16 nd; } rule_t;
typedef struct { const char *q; u32 qn; u16 parent; u8 ok; } media_t;

static rule_t *rules; static int nrules, rcap;
static int order_base = 0;               /* rules of sheet i sort after those of sheets before it */
static media_t *medias; static int nmedia, mcap;
static u16 cur_media = 0;               /* the @media block being read (0 = none) */
static int view_width = 1024;           /* the page width in device px */
static int view_height = 1800;
static u32 built_gen = 0xFFFFFFFFu;
static int built_any = 0;

/* index: rules by the key of their rightmost compound */
#define BUCKETS 2048
static int *bucket_start;               /* BUCKETS + 1 offsets into bucket_rules */
static int *bucket_rules;
static int *univ_rules; static int nuniv;

void css_reset(void){
    ar_free(&rarena);
    if(rules) kfree(rules);
    if(medias) kfree(medias);
    if(bucket_start) kfree(bucket_start);
    if(bucket_rules) kfree(bucket_rules);
    if(univ_rules) kfree(univ_rules);
    rules = NULL; nrules = rcap = 0;
    medias = NULL; nmedia = mcap = 0;
    bucket_start = bucket_rules = univ_rules = NULL; nuniv = 0;
    cur_media = 0;
    built_any = 0;
}

static u32 rule_key(const compound_t *c, int *kind){
    if(c->id){ *kind = 1; return hash_n(c->id, (u32)strlen(c->id)) & (BUCKETS - 1); }
    if(c->ncls){ *kind = 2; return hash_n(c->cls[0], (u32)strlen(c->cls[0])) & (BUCKETS - 1); }
    if(c->tag){ *kind = 3; return hash_n(c->tag, (u32)strlen(c->tag)) & (BUCKETS - 1); }
    *kind = 0;
    return 0;
}

static void build_index(void){
    if(bucket_start) kfree(bucket_start);
    if(bucket_rules) kfree(bucket_rules);
    if(univ_rules) kfree(univ_rules);
    bucket_start = kmalloc(sizeof(int) * (BUCKETS + 1));
    bucket_rules = kmalloc(sizeof(int) * (u32)(nrules + 1));
    univ_rules = kmalloc(sizeof(int) * (u32)(nrules + 1));
    nuniv = 0;
    if(!bucket_start || !bucket_rules || !univ_rules) return;
    for(int i = 0; i <= BUCKETS; i++) bucket_start[i] = 0;
    for(int i = 0; i < nrules; i++){
        int kind; u32 b = rule_key(&rules[i].s.c[rules[i].s.n - 1], &kind);
        if(kind) bucket_start[b + 1]++;
        else univ_rules[nuniv++] = i;
    }
    for(int i = 0; i < BUCKETS; i++) bucket_start[i + 1] += bucket_start[i];
    int *fill = kmalloc(sizeof(int) * BUCKETS);
    if(!fill) return;
    for(int i = 0; i < BUCKETS; i++) fill[i] = bucket_start[i];
    for(int i = 0; i < nrules; i++){
        int kind; u32 b = rule_key(&rules[i].s.c[rules[i].s.n - 1], &kind);
        if(kind) bucket_rules[fill[b]++] = i;
    }
    kfree(fill);
}

static void add_rule(const char *sel, u32 sn, const char *decl, u32 dn){
    /* the declarations, once for all the selectors in the list (theme rules can hold
       thousands of custom properties) */
    static decl_t tmp[8192];
    int nd = split_decls(decl, dn, tmp, 8192);
    if(nd == 0) return;
    char *text = ar_str(&rarena, decl, dn);           /* the declarations point into this copy */
    decl_t *ds = ar_alloc(&rarena, sizeof(decl_t) * (u32)nd);
    if(!text || !ds) return;
    for(int i = 0; i < nd; i++){
        ds[i] = tmp[i];
        ds[i].name = text + (tmp[i].name - decl);
        ds[i].v = text + (tmp[i].v - decl);
    }
    u32 start = 0; int depth = 0; char q = 0;
    for(u32 i = 0; i <= sn; i++){
        if(i < sn){
            char c = sel[i];
            if(q){ if(c == q) q = 0; continue; }
            if(c == '"' || c == '\''){ q = c; continue; }
            if(c == '(' || c == '[') depth++;
            if(c == ')' || c == ']') depth--;
            if(c != ',' || depth > 0) continue;
        }
        const char *sp = sel + start; u32 n = i - start;
        trim(&sp, &n);
        start = i + 1;
        if(!n) continue;
        sel_t s;
        if(parse_selector(sp, n, &s) != 0) continue;
        if(s.c[s.n - 1].never) continue;                  /* can never match: not kept */
        if(nrules == rcap){
            int nc = rcap ? rcap * 2 : 1024;
            rule_t *g = kmalloc(sizeof(rule_t) * (u32)nc);
            if(!g) return;
            if(rules){ memcpy(g, rules, sizeof(rule_t) * (u32)nrules); kfree(rules); }
            rules = g; rcap = nc;
        }
        rules[nrules] = (rule_t){ s, order_base + nrules, cur_media, ds, (u16)(nd > 65535 ? 65535 : nd) };
        nrules++;
    }
}

static u16 add_media(const char *q, u32 qn){
    if(nmedia == 0){
        mcap = 64;
        medias = kmalloc(sizeof(media_t) * (u32)mcap);
        if(!medias){ mcap = 0; return 0; }
        medias[0] = (media_t){ "", 0, 0, 1 };
        nmedia = 1;
    }
    if(nmedia == mcap){
        if(nmedia >= 65000) return cur_media;
        media_t *g = kmalloc(sizeof(media_t) * (u32)mcap * 2);
        if(!g) return cur_media;
        memcpy(g, medias, sizeof(media_t) * (u32)nmedia);
        kfree(medias); medias = g; mcap *= 2;
    }
    medias[nmedia] = (media_t){ ar_str(&rarena, q, qn), qn, cur_media, 1 };
    return (u16)nmedia++;
}

static int eval_len_fwd(const char *v, u32 n, int base, int *px);
/* A length in an @media condition, in hundredths of a CSS px (-1 if not understood). */
static int mq_len(const char *p, const char *end){
    while(p < end && (c_ws(*p) || *p == ':')) p++;
    if(end - p > 5 && memcmp(p, "calc(", 5) == 0){
        const char *e = p; int d = 0;
        while(e < end){ if(*e == '(') d++; if(*e == ')'){ d--; if(!d){ e++; break; } } e++; }
        int px;
        if(!eval_len_fwd(p, (u32)(e - p), CSSPX(16), &px)) return -1;
        return px * 100 * DPR_DEN / DPR_NUM;
    }
    int v = 0, frac = 0, fd = 0, any = 0;
    while(p < end && c_digit(*p)){ v = v * 10 + (*p - '0'); p++; any = 1; }
    if(p < end && *p == '.'){ p++; while(p < end && c_digit(*p)){ if(fd < 2){ frac = frac * 10 + (*p - '0'); fd++; } p++; any = 1; } }
    if(!any) return -1;
    while(fd < 2){ frac *= 10; fd++; }
    int h = v * 100 + frac;
    if(p + 1 < end && (p[0] == 'e' || p[0] == 'r')) h *= 16;
    return h;
}

/* One "(feature)" or "(feature: value)" or a range "(width >= 600px)". */
static int mq_feature(const char *s, u32 n){
    char b[96]; u32 k = 0;
    for(u32 i = 0; i < n && k + 1 < sizeof b; i++) b[k++] = (char)c_lower(s[i]);
    b[k] = 0;
    const char *e = b + k;
    int vw = view_width * 100 * DPR_DEN / DPR_NUM, vh = view_height * 100 * DPR_DEN / DPR_NUM;
    /* ranges: width >= X, X <= width, X < width < Y */
    const char *wp = NULL; int is_h = 0;
    for(const char *q = b; *q; q++){
        if((starts_n(q, (u32)(e - q), "width") && (q == b || !( (q[-1] >= 'a' && q[-1] <= 'z') || q[-1] == '-'))) ){ wp = q; break; }
        if((starts_n(q, (u32)(e - q), "height") && (q == b || !( (q[-1] >= 'a' && q[-1] <= 'z') || q[-1] == '-'))) ){ wp = q; is_h = 1; break; }
    }
    if(wp && (strchr(b, '<') || strchr(b, '>'))){
        int val = is_h ? vh : vw;
        int ok = 1;
        /* left side: "X op width" */
        const char *q = b;
        while(q < wp && c_ws(*q)) q++;
        if(q < wp){
            int x = mq_len(q, wp);
            const char *op = q; while(op < wp && *op != '<' && *op != '>') op++;
            if(x < 0 && op < wp) ok = 0;                 /* not understood: the query does not hold */
            if(x >= 0 && op < wp){
                int eqs = op + 1 < wp && op[1] == '=';
                if(*op == '<') ok &= eqs ? x <= val : x < val;
                else ok &= eqs ? x >= val : x > val;
            }
        }
        const char *r = wp + (is_h ? 6 : 5);
        while(r < e && c_ws(*r)) r++;
        if(r < e && (*r == '<' || *r == '>')){
            char op = *r; int eqs = r + 1 < e && r[1] == '=';
            r += eqs ? 2 : 1;
            int x = mq_len(r, e);
            if(x < 0) ok = 0;
            if(x >= 0){
                if(op == '<') ok &= eqs ? val <= x : val < x;
                else ok &= eqs ? val >= x : val > x;
            }
        }
        return ok;
    }
    const char *colon = strchr(b, ':');
    const char *val = colon ? colon + 1 : "";
    while(c_ws(*val)) val++;
    u32 fl = colon ? (u32)(colon - b) : k;
    while(fl && c_ws(b[fl - 1])) fl--;
    #define FEAT(lit) eq_n(b, fl, lit)
    if(FEAT("min-width")){ int x = mq_len(val, e); return x >= 0 && x <= vw; }
    if(FEAT("max-width")){ int x = mq_len(val, e); return x >= 0 && x >= vw; }
    if(FEAT("width")) return mq_len(val, e) == vw;
    if(FEAT("min-height")) return mq_len(val, e) <= vh;
    if(FEAT("max-height")) return mq_len(val, e) >= vh;
    if(FEAT("min-device-width")) return mq_len(val, e) <= 192000 * DPR_DEN / DPR_NUM;
    if(FEAT("max-device-width")) return mq_len(val, e) >= 192000 * DPR_DEN / DPR_NUM;
    if(FEAT("prefers-color-scheme")) return starts_n(val, (u32)(e - val), "light");
    if(FEAT("prefers-reduced-motion")) return !starts_n(val, (u32)(e - val), "reduce");
    if(FEAT("prefers-contrast") || FEAT("forced-colors") || FEAT("inverted-colors")) return !colon || starts_n(val, (u32)(e - val), "no-preference") || starts_n(val, (u32)(e - val), "none");
    if(FEAT("hover") || FEAT("any-hover")) return !colon || starts_n(val, (u32)(e - val), "hover");
    if(FEAT("pointer") || FEAT("any-pointer")) return !colon || starts_n(val, (u32)(e - val), "fine");
    if(FEAT("orientation")) return starts_n(val, (u32)(e - val), "landscape");
    if(FEAT("display-mode")) return starts_n(val, (u32)(e - val), "browser");
    if(FEAT("min-resolution") || FEAT("-webkit-min-device-pixel-ratio") || FEAT("min--moz-device-pixel-ratio")){
        int x = mq_len(val, e);                         /* 2dppx, 192dpi or a bare ratio */
        if(strstr(val, "dpi")) x = x / 96;
        return x <= DPR_NUM * 100 / DPR_DEN;
    }
    if(FEAT("max-resolution") || FEAT("-webkit-max-device-pixel-ratio")){
        int x = mq_len(val, e);
        if(strstr(val, "dpi")) x = x / 96;
        return x >= DPR_NUM * 100 / DPR_DEN;
    }
    if(FEAT("update")) return !colon || starts_n(val, (u32)(e - val), "fast");
    if(FEAT("scripting")) return !colon || starts_n(val, (u32)(e - val), "enabled");
    #undef FEAT
    return 1;                                            /* anything else: assume it holds */
}

/* A whole media query list: "screen and (min-width: 40em), print". */
static int mq_eval(const char *q, u32 n){
    if(n == 0) return 1;
    u32 start = 0; int depth = 0;
    for(u32 i = 0; i <= n; i++){
        if(i < n){
            if(q[i] == '(') depth++;
            if(q[i] == ')') depth--;
            if(q[i] != ',' || depth > 0) continue;
        }
        /* one query */
        const char *s = q + start; u32 sn = i - start;
        start = i + 1;
        trim(&s, &sn);
        int neg = 0, ok = 1, combine_or = 0, first = 1;
        u32 j = 0;
        while(j < sn){
            while(j < sn && c_ws(s[j])) j++;
            if(j >= sn) break;
            if(s[j] == '('){
                int d = 0; u32 b = j;
                while(j < sn){ if(s[j] == '(') d++; if(s[j] == ')'){ d--; if(d == 0) break; } j++; }
                const char *in = s + b + 1; u32 inl = j > b + 1 ? j - b - 1 : 0;
                if(j < sn) j++;
                int v;
                /* nested "(a) and (b)" or "not (a)" inside parentheses */
                const char *ip = in; u32 il = inl; trim(&ip, &il);
                if(il && ip[0] == '(') v = mq_eval(ip, il);
                else if(il > 4 && starts_n(ip, il, "not ")){ v = !mq_eval(ip + 4, il - 4); }
                else v = mq_feature(in, inl);
                if(first) ok = v; else ok = combine_or ? (ok || v) : (ok && v);
                first = 0;
                continue;
            }
            u32 b = j;
            while(j < sn && !c_ws(s[j]) && s[j] != '(') j++;
            const char *w = s + b; u32 wl = j - b;
            char lw[16]; u32 lk = 0;
            for(u32 t = 0; t < wl && lk + 1 < sizeof lw; t++) lw[lk++] = (char)c_lower(w[t]);
            lw[lk] = 0;
            if(strcmp(lw, "not") == 0) neg = 1;
            else if(strcmp(lw, "only") == 0) { }
            else if(strcmp(lw, "and") == 0) combine_or = 0;
            else if(strcmp(lw, "or") == 0) combine_or = 1;
            else if(strcmp(lw, "screen") == 0 || strcmp(lw, "all") == 0){ if(first) ok = 1; first = 0; }
            else { ok = 0; first = 0; }                   /* print, speech, tv... */
        }
        if(neg) ok = !ok;
        if(ok) return 1;
    }
    return 0;
}

/* The index just past the block that starts at the '{' at i. */
static u32 skip_block(const char *t, u32 len, u32 i){
    int depth = 0; char q = 0;
    while(i < len){
        char c = t[i];
        if(q){ if(c == '\\') i++; else if(c == q) q = 0; i++; continue; }
        if(c == '"' || c == '\'') q = c;
        else if(c == '{') depth++;
        else if(c == '}'){ depth--; if(depth == 0) return i + 1; }
        i++;
    }
    return len;
}

void css_add_sheet(const char *t, u32 len){
    u32 i = 0;
    while(i < len){
        if(i + 1 < len && t[i] == '/' && t[i+1] == '*'){
            i += 2;
            while(i + 1 < len && !(t[i] == '*' && t[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if(c_ws(t[i]) || t[i] == '<' || t[i] == '>' || t[i] == '}' || t[i] == ';'){ i++; continue; }
        if(t[i] == '<' && i + 3 < len && t[i+1] == '!' && t[i+2] == '-' && t[i+3] == '-'){ i += 4; continue; }
        if(t[i] == '-' && i + 2 < len && t[i+1] == '-' && t[i+2] == '>'){ i += 3; continue; }
        if(t[i] == '@'){
            u32 a = i;
            while(i < len && t[i] != ';' && t[i] != '{') i++;
            if(i >= len) break;
            if(t[i] == ';'){ i++; continue; }          /* @import, @charset... */
            u32 open = i;
            u32 close = skip_block(t, len, i);
            const char *at = t + a; u32 al = open - a;
            if(starts_n(at, al, "@media")){
                u16 saved = cur_media;
                cur_media = add_media(at + 6, al - 6);
                css_add_sheet(t + open + 1, close - open - 2);
                cur_media = saved;
            } else if(starts_n(at, al, "@supports")){
                const char *c = at + 9; u32 cl = al - 9;
                trim(&c, &cl);
                int ok = !(cl >= 3 && starts_n(c, cl, "not"));
                if(ok) css_add_sheet(t + open + 1, close - open - 2);
            } else if(starts_n(at, al, "@layer") || starts_n(at, al, "@container") || starts_n(at, al, "@document") ||
                      starts_n(at, al, "@scope") || starts_n(at, al, "@-moz-document")){
                css_add_sheet(t + open + 1, close - open - 2);
            }
            i = close;                                  /* @font-face, @keyframes, @page...: skipped */
            continue;
        }
        u32 ss = i;
        /* the selector runs to '{' (outside strings and brackets) */
        int depth = 0; char q = 0;
        while(i < len){
            char c = t[i];
            if(q){ if(c == q) q = 0; i++; continue; }
            if(c == '"' || c == '\''){ q = c; i++; continue; }
            if(c == '(' || c == '[') depth++;
            else if(c == ')' || c == ']') depth--;
            else if((c == '{' || c == '}') && depth <= 0) break;
            i++;
        }
        if(i >= len) break;
        if(t[i] == '}'){ i++; continue; }              /* a stray '}' */
        u32 se = i;
        u32 close = skip_block(t, len, i);
        u32 ds = i + 1, de = close > ds ? close - 1 : ds;
        /* nested rules (CSS nesting) are not supported: a block with '{' inside is cut there */
        for(u32 k = ds; k < de; k++) if(t[k] == '{'){ while(k > ds && t[k] != ';' && t[k] != '}') k--; de = k; break; }
        i = close;
        const char *sp = t + ss; u32 sn = se - ss;
        trim(&sp, &sn);
        if(sn) add_rule(sp, sn, t + ds, de - ds);
    }
}

/* ================= custom properties (var) ================= */

#define VSLOTS 4096
typedef struct { const char *name; u16 nl; int top; } vslot_t;
typedef struct { const char *val; u32 vl; int slot, prev; } ventry_t;
static vslot_t vslots[VSLOTS];
static int vused[VSLOTS], nvused;
static ventry_t *vents; static int nvent, vcap;

static void var_reset(void){
    for(int i = 0; i < nvused; i++){ vslots[vused[i]].name = NULL; vslots[vused[i]].top = -1; }
    nvused = 0;
    nvent = 0;
}
int css_var_mark(void){ return nvent; }
void css_var_pop(int mark){
    while(nvent > mark){
        nvent--;
        vslots[vents[nvent].slot].top = vents[nvent].prev;
    }
}
static int var_slot(const char *name, u32 nl, int create){
    u32 h = hash_n(name, nl) & (VSLOTS - 1);
    for(u32 probe = 0; probe < VSLOTS; probe++){
        vslot_t *s = &vslots[h];
        if(!s->name){
            if(!create || nvused >= VSLOTS - 64) return -1;
            s->name = name; s->nl = (u16)nl; s->top = -1;
            vused[nvused++] = (int)h;
            return (int)h;
        }
        if(s->nl == nl && memcmp(s->name, name, nl) == 0) return (int)h;
        h = (h + 1) & (VSLOTS - 1);
    }
    return -1;
}
static void var_set(const char *name, u32 nl, const char *val, u32 vl){
    int s = var_slot(name, nl, 1);
    if(s < 0) return;
    if(nvent == vcap){
        int nc = vcap ? vcap * 2 : 4096;
        ventry_t *g = kmalloc(sizeof(ventry_t) * (u32)nc);
        if(!g) return;
        if(vents){ memcpy(g, vents, sizeof(ventry_t) * (u32)nvent); kfree(vents); }
        vents = g; vcap = nc;
    }
    vents[nvent] = (ventry_t){ val, vl, s, vslots[s].top };
    vslots[s].top = nvent++;
}
static int var_get(const char *name, u32 nl, const char **val, u32 *vl){
    int s = var_slot(name, nl, 0);
    if(s < 0 || vslots[s].top < 0) return 0;
    *val = vents[vslots[s].top].val; *vl = vents[vslots[s].top].vl;
    return 1;
}

/* Copies v into out with every var(--x, fallback) replaced. Returns the length, or -1 when a
 * variable is missing and has no fallback (the declaration is then invalid). */
static int subst(const char *v, u32 n, char *out, u32 cap, int depth){
    u32 k = 0;
    for(u32 i = 0; i < n; ){
        if(i + 4 <= n && v[i] == 'v' && v[i+1] == 'a' && v[i+2] == 'r' && v[i+3] == '(' &&
           (i == 0 || !((v[i-1] >= 'a' && v[i-1] <= 'z') || v[i-1] == '-'))){
            u32 j = i + 4, d = 1;
            u32 inner = j;
            while(j < n && d){ if(v[j] == '(') d++; else if(v[j] == ')') d--; if(d) j++; }
            /* var( name [, fallback] ) */
            u32 comma = inner; int dd = 0;
            while(comma < j && !(v[comma] == ',' && dd == 0)){ if(v[comma] == '(') dd++; if(v[comma] == ')') dd--; comma++; }
            const char *nm = v + inner; u32 nl = comma - inner;
            trim(&nm, &nl);
            const char *val; u32 vl;
            int r;
            if(depth < 8 && var_get(nm, nl, &val, &vl)) r = subst(val, vl, out + k, cap - k, depth + 1);
            else if(comma < j) { const char *fb = v + comma + 1; u32 fl = j - comma - 1; trim(&fb, &fl); r = subst(fb, fl, out + k, cap - k, depth + 1); }
            else r = -1;
            if(r < 0) return -1;
            k += (u32)r;
            i = j < n ? j + 1 : n;
            continue;
        }
        if(k + 1 < cap) out[k++] = v[i];
        i++;
    }
    if(k < cap) out[k] = 0;
    return (int)k;
}

/* ================= values ================= */

typedef struct { const char *name; u32 rgb; } named_t;
static const named_t colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "navy", 0x000080 }, { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "purple", 0x800080 },
    { "teal", 0x008080 }, { "maroon", 0x800000 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF },
    { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "olive", 0x808000 },
    { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "gold", 0xFFD700 }, { "indigo", 0x4B0082 },
    { "violet", 0xEE82EE }, { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "tomato", 0xFF6347 },
    { "crimson", 0xDC143C }, { "khaki", 0xF0E68C }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 },
    { "linen", 0xFAF0E6 }, { "snow", 0xFFFAFA }, { "azure", 0xF0FFFF }, { "lavender", 0xE6E6FA },
    { "tan", 0xD2B48C }, { "plum", 0xDDA0DD }, { "orchid", 0xDA70D6 }, { "turquoise", 0x40E0D0 },
    { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 }, { "dodgerblue", 0x1E90FF },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "gainsboro", 0xDCDCDC }, { "whitesmoke", 0xF5F5F5 }, { "ghostwhite", 0xF8F8FF }, { "aliceblue", 0xF0F8FF },
    { "honeydew", 0xF0FFF0 }, { "mintcream", 0xF5FFFA }, { "seashell", 0xFFF5EE }, { "oldlace", 0xFDF5E6 },
    { "floralwhite", 0xFFFAF0 }, { "cornsilk", 0xFFF8DC }, { "lemonchiffon", 0xFFFACD }, { "lightyellow", 0xFFFFE0 },
    { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 }, { "lightpink", 0xFFB6C1 }, { "lightcyan", 0xE0FFFF },
    { "darkblue", 0x00008B }, { "darkgreen", 0x006400 }, { "darkred", 0x8B0000 }, { "darkorange", 0xFF8C00 },
    { "darkviolet", 0x9400D3 }, { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F }, { "midnightblue", 0x191970 },
    { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "limegreen", 0x32CD32 }, { "firebrick", 0xB22222 },
    { "chocolate", 0xD2691E }, { "sienna", 0xA0522D }, { "peru", 0xCD853F }, { "goldenrod", 0xDAA520 },
    { "wheat", 0xF5DEB3 }, { "mistyrose", 0xFFE4E1 }, { "papayawhip", 0xFFEFD5 }, { "moccasin", 0xFFE4B5 },
    { "rebeccapurple", 0x663399 }, { "cornflowerblue", 0x6495ED }, { "cadetblue", 0x5F9EA0 }, { "hotpink", 0xFF69B4 },
    { "deeppink", 0xFF1493 }, { "orangered", 0xFF4500 }, { "yellowgreen", 0x9ACD32 }, { "chartreuse", 0x7FFF00 },
    { "springgreen", 0x00FF7F }, { "aquamarine", 0x7FFFD4 }, { "lightsteelblue", 0xB0C4DE }, { "powderblue", 0xB0E0E6 },
    { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "navajowhite", 0xFFDEAD }, { "bisque", 0xFFE4C4 },
    { "antiquewhite", 0xFAEBD7 }, { "blanchedalmond", 0xFFEBCD }, { "burlywood", 0xDEB887 }, { "rosybrown", 0xBC8F8F },
    { "darkkhaki", 0xBDB76B }, { "olivedrab", 0x6B8E23 }, { "darkolivegreen", 0x556B2F }, { "mediumblue", 0x0000CD },
    { "darkcyan", 0x008B8B }, { "lightseagreen", 0x20B2AA }, { "darkslateblue", 0x483D8B }, { "slateblue", 0x6A5ACD },
    { "mediumpurple", 0x9370DB }, { "darkmagenta", 0x8B008B }, { "mediumvioletred", 0xC71585 }, { "palevioletred", 0xDB7093 },
    { "lightcoral", 0xF08080 }, { "indianred", 0xCD5C5C }, { "darksalmon", 0xE9967A }, { "lightsalmon", 0xFFA07A },
    { "palegreen", 0x98FB98 }, { "mediumseagreen", 0x3CB371 }, { "darkseagreen", 0x8FBC8F }, { "palegoldenrod", 0xEEE8AA },
    { "lightgoldenrodyellow", 0xFAFAD2 }, { "thistle", 0xD8BFD8 }, { "lavenderblush", 0xFFF0F5 }, { "peachpuff", 0xFFDAB9 },
    { "deepskyblue", 0x00BFFF }, { "lightskyblue", 0x87CEFA }, { "mediumturquoise", 0x48D1CC }, { "darkturquoise", 0x00CED1 },
    { "paleturquoise", 0xAFEEEE }, { "mediumaquamarine", 0x66CDAA }, { "blueviolet", 0x8A2BE2 }, { "darkorchid", 0x9932CC },
    { "mediumorchid", 0xBA55D3 }, { "greenyellow", 0xADFF2F }, { "lawngreen", 0x7CFC00 }, { "mediumspringgreen", 0x00FA9A },
    { "saddlebrown", 0x8B4513 }, { "darkgoldenrod", 0xB8860B }, { "sandybrown", 0xF4A460 },
    /* system colours */
    { "canvas", 0xFFFFFF }, { "canvastext", 0x000000 }, { "linktext", 0x0000EE }, { "buttonface", 0xE9E9ED },
    { "buttontext", 0x000000 }, { "field", 0xFFFFFF }, { "fieldtext", 0x000000 }, { "graytext", 0x6D6D6D },
    { "highlight", 0x3390FF }, { "highlighttext", 0xFFFFFF }, { "window", 0xFFFFFF }, { "windowtext", 0x000000 },
};

#define COL_TRANSPARENT 0xFFFFFFFFu
#define COL_INHERIT     0xFFFFFFFEu
#define COL_CURRENT     0xFFFFFFFDu

static u32 blend(u32 over, u32 under, int a256){
    if(a256 >= 256) return over;
    if(a256 <= 0) return under;
    u32 r = (((over >> 16) & 255) * (u32)a256 + ((under >> 16) & 255) * (u32)(256 - a256)) >> 8;
    u32 g = (((over >> 8) & 255) * (u32)a256 + ((under >> 8) & 255) * (u32)(256 - a256)) >> 8;
    u32 b = ((over & 255) * (u32)a256 + (under & 255) * (u32)(256 - a256)) >> 8;
    return (r << 16) | (g << 8) | b;
}

/* A number with optional fraction, in thousandths; *unit gets what follows. 0 if none. */
static int num_milli(const char *v, u32 n, u32 *i, int *out){
    int neg = 0, any = 0;
    if(*i < n && (v[*i] == '-' || v[*i] == '+')){ neg = v[*i] == '-'; (*i)++; }
    int whole = 0, frac = 0, fd = 0;
    while(*i < n && c_digit(v[*i])){ if(whole < 10000000) whole = whole * 10 + (v[*i] - '0'); (*i)++; any = 1; }
    if(*i < n && v[*i] == '.'){
        (*i)++;
        while(*i < n && c_digit(v[*i])){ if(fd < 3){ frac = frac * 10 + (v[*i] - '0'); fd++; } (*i)++; any = 1; }
    }
    while(fd < 3){ frac *= 10; fd++; }
    if(*i < n && (v[*i] == 'e' || v[*i] == 'E') && *i + 1 < n && (c_digit(v[*i + 1]) || v[*i + 1] == '-')){
        /* 1e-05 and the like: tiny or huge, rounded */
        (*i)++;
        int en = 0, eneg = 0;
        if(v[*i] == '-'){ eneg = 1; (*i)++; }
        while(*i < n && c_digit(v[*i])){ en = en * 10 + (v[*i] - '0'); (*i)++; }
        long long m = (long long)whole * 1000 + frac;
        while(en-- > 0){ if(eneg) m /= 10; else if(m < 1000000000LL) m *= 10; }
        whole = (int)(m / 1000); frac = (int)(m % 1000);
    }
    if(!any) return 0;
    long long m = (long long)whole * 1000 + frac;
    if(m > 2000000000LL) m = 2000000000LL;
    *out = neg ? -(int)m : (int)m;
    return 1;
}

/* Parses a colour. under is what shows through a translucent colour. */
static int parse_color(const char *v, u32 n, u32 *out, u32 under){
    trim(&v, &n);
    if(n == 0) return 0;
    if(v[0] == '#'){
        u32 d[8]; int nd = 0;
        for(u32 i = 1; i < n; i++){ int h = hexv(v[i]); if(h < 0) return 0; if(nd < 8) d[nd++] = (u32)h; }
        u32 r, g, b, a = 255;
        if(nd == 3 || nd == 4){ r = d[0] * 17; g = d[1] * 17; b = d[2] * 17; if(nd == 4) a = d[3] * 17; }
        else if(nd == 6 || nd == 8){ r = d[0] * 16 + d[1]; g = d[2] * 16 + d[3]; b = d[4] * 16 + d[5]; if(nd == 8) a = d[6] * 16 + d[7]; }
        else return 0;
        if(a < 10){ *out = COL_TRANSPARENT; return 1; }
        *out = blend((r << 16) | (g << 8) | b, under, (int)(a * 256 / 255));
        return 1;
    }
    int is_rgb = n >= 4 && c_lower(v[0]) == 'r' && c_lower(v[1]) == 'g' && c_lower(v[2]) == 'b';
    int is_hsl = n >= 4 && c_lower(v[0]) == 'h' && c_lower(v[1]) == 's' && c_lower(v[2]) == 'l';
    if(is_rgb || is_hsl){
        u32 i = 3;
        if(i < n && c_lower(v[i]) == 'a') i++;
        if(i >= n || v[i] != '(') return 0;
        i++;
        int comp[4], pct[4], nc = 0;
        while(i < n && nc < 4){
            while(i < n && (c_ws(v[i]) || v[i] == ',' || v[i] == '/')) i++;
            if(i >= n || v[i] == ')') break;
            int m;
            if(!num_milli(v, n, &i, &m)){
                /* "none" or a unit word: skip it */
                while(i < n && !c_ws(v[i]) && v[i] != ',' && v[i] != ')' && v[i] != '/') i++;
                comp[nc] = 0; pct[nc++] = 0;
                continue;
            }
            pct[nc] = 0;
            if(i < n && v[i] == '%'){ pct[nc] = 1; i++; }
            else if(i + 2 < n && v[i] == 'd' && v[i+1] == 'e' && v[i+2] == 'g') i += 3;
            else if(i + 3 < n && v[i] == 't' && v[i+1] == 'u') { m *= 360; i += 4; }
            comp[nc++] = m;
        }
        if(nc < 3) return 0;
        int alpha = 256;
        if(nc == 4) alpha = pct[3] ? comp[3] * 256 / 100000 : comp[3] * 256 / 1000;
        if(alpha < 10){ *out = COL_TRANSPARENT; return 1; }
        u32 rgb;
        if(is_rgb){
            int c3[3];
            for(int j = 0; j < 3; j++){
                int x = pct[j] ? comp[j] * 255 / 100000 : comp[j] / 1000;
                c3[j] = x < 0 ? 0 : x > 255 ? 255 : x;
            }
            rgb = ((u32)c3[0] << 16) | ((u32)c3[1] << 8) | (u32)c3[2];
        } else {
            /* hsl: h in degrees, s and l in percent (thousandths of a percent here) */
            int h = (comp[0] / 1000) % 360; if(h < 0) h += 360;
            int s = comp[1] / 100, l = comp[2] / 100;          /* tenths of a percent */
            if(s < 0) s = 0; if(s > 1000) s = 1000; if(l < 0) l = 0; if(l > 1000) l = 1000;
            int c = (1000 - (2 * l - 1000 < 0 ? 1000 - 2 * l : 2 * l - 1000)) * s / 1000;   /* chroma, 0..1000 */
            int hp = h * 1000 / 60;                             /* h / 60, in thousandths */
            int mod2 = hp % 2000;
            int x = c * (1000 - (mod2 - 1000 < 0 ? 1000 - mod2 : mod2 - 1000)) / 1000;
            int r1 = 0, g1 = 0, b1 = 0;
            if(h < 60){ r1 = c; g1 = x; } else if(h < 120){ r1 = x; g1 = c; } else if(h < 180){ g1 = c; b1 = x; }
            else if(h < 240){ g1 = x; b1 = c; } else if(h < 300){ r1 = x; b1 = c; } else { r1 = c; b1 = x; }
            int m = l - c / 2;
            int R = (r1 + m) * 255 / 1000, G = (g1 + m) * 255 / 1000, B = (b1 + m) * 255 / 1000;
            R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
            rgb = ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
        *out = blend(rgb, under, alpha);
        return 1;
    }
    char low[32]; u32 k = 0;
    if(n >= sizeof low) return 0;
    for(u32 i = 0; i < n; i++) low[k++] = (char)c_lower(v[i]);
    low[k] = 0;
    if(strcmp(low, "transparent") == 0){ *out = COL_TRANSPARENT; return 1; }
    if(strcmp(low, "inherit") == 0){ *out = COL_INHERIT; return 1; }
    if(strcmp(low, "currentcolor") == 0){ *out = COL_CURRENT; return 1; }
    for(u32 i = 0; i < sizeof colors / sizeof colors[0]; i++)
        if(strcmp(low, colors[i].name) == 0){ *out = colors[i].rgb; return 1; }
    return 0;
}

/* Lengths. A value is px (device) + tenths of a percent of the container + of the viewport. */
static int root_size = CSSPX(16);          /* the font size of <html>, for rem */
typedef struct { int px, pct, vw; int ok; } len_t;

static int unit_px(int milli, const char *u, u32 ul, int base, int *px, int *pct, int *vw){
    /* milli: the number in thousandths */
    #define U(lit) eq_n(u, ul, lit)
    *px = *pct = *vw = 0;
    if(ul == 0 || U("px")) { *px = (int)((long long)milli * DPR_NUM / DPR_DEN / 1000); return 1; }
    if(U("em")) { *px = (int)((long long)milli * base / 1000); return 1; }
    if(U("rem")){ *px = (int)((long long)milli * root_size / 1000); return 1; }
    if(U("%"))  { *pct = milli / 100; return 1; }
    if(U("vw") || U("vi") || U("svw") || U("lvw") || U("dvw") || U("cqw") || U("cqi")) { *vw = milli / 100; return 1; }
    if(U("vh") || U("svh") || U("lvh") || U("dvh") || U("vb")) { *px = (int)((long long)milli * view_height / 100000); return 1; }
    if(U("vmin")) { *px = (int)((long long)milli * (view_height < view_width ? view_height : view_width) / 100000); return 1; }
    if(U("vmax")) { *px = (int)((long long)milli * (view_height > view_width ? view_height : view_width) / 100000); return 1; }
    if(U("pt")) { *px = (int)((long long)milli * 4 * DPR_NUM / DPR_DEN / 3000); return 1; }
    if(U("pc")) { *px = (int)((long long)milli * 16 * DPR_NUM / DPR_DEN / 1000); return 1; }
    if(U("in")) { *px = (int)((long long)milli * 96 * DPR_NUM / DPR_DEN / 1000); return 1; }
    if(U("cm")) { *px = (int)((long long)milli * 3780 * DPR_NUM / DPR_DEN / 100000); return 1; }
    if(U("mm")) { *px = (int)((long long)milli * 378 * DPR_NUM / DPR_DEN / 100000); return 1; }
    if(U("ch") || U("ex") || U("ic") || U("cap")) { *px = (int)((long long)milli * base / 2000); return 1; }
    if(U("lh") || U("rlh")) { *px = (int)((long long)milli * base * 12 / 10000); return 1; }
    if(U("fr")) { return 0; }
    #undef U
    return 0;
}

static int eval_len(const char *v, u32 n, int base, len_t *out);

/* calc() and friends: + - * / over lengths and plain numbers. */
static int eval_sum(const char *v, u32 n, int base, len_t *out, int *is_num, int *numv);

static int eval_term(const char *v, u32 n, int base, len_t *out, int *is_num, int *numv){
    trim(&v, &n);
    out->px = out->pct = out->vw = 0; out->ok = 0; *is_num = 0;
    if(!n) return 0;
    if(v[0] == '(' && v[n - 1] == ')') return eval_sum(v + 1, n - 2, base, out, is_num, numv);
    /* functions */
    u32 fn = 0;
    while(fn < n && ((v[fn] >= 'a' && v[fn] <= 'z') || v[fn] == '-')) fn++;
    if(fn && fn < n && v[fn] == '(' && v[n - 1] == ')'){
        const char *a = v + fn + 1; u32 al = n - fn - 2;
        if(eq_n(v, fn, "calc") || eq_n(v, fn, "-webkit-calc")) return eval_sum(a, al, base, out, is_num, numv);
        if(eq_n(v, fn, "min") || eq_n(v, fn, "max") || eq_n(v, fn, "clamp")){
            len_t args[6]; int na = 0;
            u32 s = 0; int d = 0;
            for(u32 i = 0; i <= al && na < 6; i++){
                if(i < al){ if(a[i] == '(') d++; if(a[i] == ')') d--; if(a[i] != ',' || d) continue; }
                int isn, nv;
                if(eval_sum(a + s, i - s, base, &args[na], &isn, &nv)) na++;
                s = i + 1;
            }
            if(!na) return 0;
            int pick = 0;
            int is_clamp = eq_n(v, fn, "clamp"), is_min = eq_n(v, fn, "min");
            /* comparable when no percentages are involved; otherwise prefer the fixed one */
            int any_pct = 0;
            for(int k = 0; k < na; k++) if(args[k].pct) any_pct = 1;
            if(is_clamp && na == 3){
                if(any_pct){ *out = args[1]; return 1; }
                int lo = args[0].px + view_width * args[0].vw / 1000, mid = args[1].px + view_width * args[1].vw / 1000,
                    hi = args[2].px + view_width * args[2].vw / 1000;
                int r = mid < lo ? lo : mid > hi ? hi : mid;
                *out = (len_t){ r, 0, 0, 1 };
                return 1;
            }
            if(any_pct){
                for(int k = 0; k < na; k++) if(!args[k].pct){ pick = k; break; }
            } else {
                for(int k = 1; k < na; k++){
                    int x = args[k].px + view_width * args[k].vw / 1000, y = args[pick].px + view_width * args[pick].vw / 1000;
                    if(is_min ? x < y : x > y) pick = k;
                }
            }
            *out = args[pick];
            return 1;
        }
        if(eq_n(v, fn, "env")){
            /* env(safe-area-inset-left, 0px): the fallback */
            u32 c = 0; while(c < al && a[c] != ',') c++;
            if(c < al) return eval_len(a + c + 1, al - c - 1, base, out);
            out->ok = 1; return 1;
        }
        return 0;
    }
    u32 i = 0; int m;
    if(!num_milli(v, n, &i, &m)) return 0;
    const char *u = v + i; u32 ul = n - i;
    if(ul == 0){ *is_num = 1; *numv = m; out->px = (int)((long long)m * DPR_NUM / DPR_DEN / 1000); out->ok = 1; return 1; }
    char lu[8]; u32 k = 0;
    if(ul >= sizeof lu) return 0;
    for(u32 j = 0; j < ul; j++) lu[k++] = (char)c_lower(u[j]);
    if(!unit_px(m, lu, ul, base, &out->px, &out->pct, &out->vw)) return 0;
    out->ok = 1;
    return 1;
}

static int eval_sum(const char *v, u32 n, int base, len_t *out, int *is_num, int *numv){
    trim(&v, &n);
    *out = (len_t){ 0, 0, 0, 0 };
    *is_num = 1; *numv = 0;
    /* terms split by + and - at depth 0 with spaces around (calc syntax); products inside */
    u32 s = 0; int d = 0; int sign = 1; int first = 1;
    for(u32 i = 0; i <= n; i++){
        int split = 0;
        if(i < n){
            if(v[i] == '(') d++;
            if(v[i] == ')') d--;
            if(d == 0 && (v[i] == '+' || v[i] == '-') && i > 0 && c_ws(v[i - 1]) && i + 1 < n && c_ws(v[i + 1])) split = 1;
            if(!split) continue;
        }
        /* a product: a * b / c */
        const char *t = v + s; u32 tl = i - s;
        trim(&t, &tl);
        if(tl){
            len_t acc = { 0, 0, 0, 0 }; int acc_num = 1, acc_v = 1000; int have = 0;
            u32 ps = 0; int pd = 0; char op = '*';
            for(u32 j = 0; j <= tl; j++){
                if(j < tl){
                    if(t[j] == '(') pd++;
                    if(t[j] == ')') pd--;
                    if(pd || (t[j] != '*' && t[j] != '/')) continue;
                }
                len_t f; int fnum, fv = 0;
                if(!eval_term(t + ps, j - ps, base, &f, &fnum, &fv)) return 0;
                if(!have){ acc = f; acc_num = fnum; acc_v = fv; have = 1; }
                else if(op == '*'){
                    if(fnum && !acc_num){ acc.px = (int)((long long)acc.px * fv / 1000); acc.pct = (int)((long long)acc.pct * fv / 1000); acc.vw = (int)((long long)acc.vw * fv / 1000); }
                    else if(acc_num && !fnum){ f.px = (int)((long long)f.px * acc_v / 1000); f.pct = (int)((long long)f.pct * acc_v / 1000); f.vw = (int)((long long)f.vw * acc_v / 1000); acc = f; acc_num = 0; }
                    else if(acc_num && fnum){ acc_v = (int)((long long)acc_v * fv / 1000); acc.px = (int)((long long)acc_v * DPR_NUM / DPR_DEN / 1000); }
                } else {
                    if(!fnum || fv == 0) return 0;
                    if(acc_num){ acc_v = (int)((long long)acc_v * 1000 / fv); acc.px = (int)((long long)acc_v * DPR_NUM / DPR_DEN / 1000); }
                    else { acc.px = (int)((long long)acc.px * 1000 / fv); acc.pct = (int)((long long)acc.pct * 1000 / fv); acc.vw = (int)((long long)acc.vw * 1000 / fv); }
                }
                if(j < tl) op = t[j];
                ps = j + 1;
            }
            if(!have) return 0;
            if(!acc_num) *is_num = 0;
            out->px += sign * acc.px; out->pct += sign * acc.pct; out->vw += sign * acc.vw;
            if(acc_num) *numv += sign * acc_v;
            out->ok = 1;
            first = 0;
        }
        if(i < n) sign = v[i] == '-' ? -1 : 1;
        s = i + 1;
    }
    (void)first;
    return out->ok;
}

/* A length: "12px", "1.5em", "50%", "calc(100% - 2rem)", "0". */
static int eval_len(const char *v, u32 n, int base, len_t *out){
    int isn, nv;
    if(!eval_sum(v, n, base, out, &isn, &nv)) return 0;
    if(isn && nv != 0){ /* a bare number other than 0 is not a length (except line-height-like uses) */ }
    return 1;
}
/* Just px (percentages of the font size for font-size). */
static int parse_len(const char *v, u32 n, int base, int *out){
    len_t l;
    if(!eval_len(v, n, base, &l)) return 0;
    *out = l.px + (int)((long long)base * l.pct / 1000) + view_width * l.vw / 1000;
    return 1;
}

static int eval_len_fwd(const char *v, u32 n, int base, int *px){ return parse_len(v, n, base, px); }

static int word_eq(const char *v, const char *lit){ return strcmp(v, lit) == 0; }

/* Splits v at spaces outside parentheses into up to max words. */
static int words(const char *v, u32 n, const char **w, u32 *wl, int max){
    int k = 0; u32 i = 0;
    while(i < n && k < max){
        while(i < n && (c_ws(v[i]) || v[i] == ',')) i++;
        if(i >= n) break;
        u32 b = i; int d = 0;
        while(i < n && (d > 0 || (!c_ws(v[i]) && v[i] != ','))){ if(v[i] == '(' || v[i] == '[') d++; if(v[i] == ')' || v[i] == ']') d--; i++; }
        w[k] = v + b; wl[k] = i - b; k++;
    }
    return k;
}

static void parse_box(const char *v, u32 n, int base, int side[4], int *autos){
    const char *w[4]; u32 wl[4];
    int cnt = words(v, n, w, wl, 4);
    int vals[4], au[4];
    for(int k = 0; k < cnt; k++){
        au[k] = eq_n(w[k], wl[k], "auto");
        vals[k] = 0;
        if(!au[k] && !parse_len(w[k], wl[k], base, &vals[k])) return;
    }
    if(cnt == 0) return;
    int map[4];
    if(cnt == 1){ map[0] = map[1] = map[2] = map[3] = 0; }
    else if(cnt == 2){ map[0] = map[2] = 0; map[1] = map[3] = 1; }
    else if(cnt == 3){ map[0] = 0; map[1] = map[3] = 1; map[2] = 2; }
    else { map[0] = 0; map[1] = 1; map[2] = 2; map[3] = 3; }
    for(int s = 0; s < 4; s++) side[s] = vals[map[s]];
    if(autos) *autos = (au[map[1]] ? 1 : 0) | (au[map[3]] ? 2 : 0);
}

static int named_size(const char *v, int parent, int *px){
    static const struct { const char *n; int css; } sz[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 },
        { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 }
    };
    for(u32 i = 0; i < sizeof sz / sizeof sz[0]; i++)
        if(strcmp(v, sz[i].n) == 0){ *px = CSSPX(sz[i].css); return 1; }
    if(strcmp(v, "smaller") == 0){ *px = parent * 5 / 6; return 1; }
    if(strcmp(v, "larger") == 0){ *px = parent * 6 / 5; return 1; }
    return 0;
}

static void set_border_side(wstyle_t *st, int side, int px){
    if(side == 0) st->bt = px; else if(side == 1) st->br = px; else if(side == 2) st->bb = px; else st->bl = px;
}
static int border_width_word(const char *w, u32 wl, int base, int *px){
    if(eq_n(w, wl, "thin")){ *px = CSSPX(1); return 1; }
    if(eq_n(w, wl, "medium")){ *px = CSSPX(3); return 1; }
    if(eq_n(w, wl, "thick")){ *px = CSSPX(5); return 1; }
    if(w[0] == '-' || w[0] == '.' || c_digit(w[0]) || starts_n(w, wl, "calc(")){
        if(!parse_len(w, wl, base, px)) return 0;
        if(*px < 0) *px = 0;
        return 1;
    }
    return 0;
}
static int border_style_word(const char *w, u32 wl){
    /* 1 a visible style, 0 none/hidden, -1 not a style word */
    static const char *vis[] = { "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset", 0 };
    if(eq_n(w, wl, "none") || eq_n(w, wl, "hidden")) return 0;
    for(int i = 0; vis[i]; i++) if(eq_n(w, wl, vis[i])) return 1;
    return -1;
}

/* border / border-top...: "1px solid #ccc". side -1 = all four. */
static void border_short(wstyle_t *st, int side, const char *v, u32 n){
    const char *w[4]; u32 wl[4];
    int cnt = words(v, n, w, wl, 4);
    int width = CSSPX(3), style = 0, have_w = 0;
    u32 color = st->color;
    for(int k = 0; k < cnt; k++){
        int px, s; u32 c;
        if((s = border_style_word(w[k], wl[k])) >= 0) style = s;
        else if(border_width_word(w[k], wl[k], st->size, &px)){ width = px; have_w = 1; }
        else if(parse_color(w[k], wl[k], &c, st->under)){ if(c == COL_CURRENT || c == COL_INHERIT) c = st->color; color = c != COL_TRANSPARENT ? c : (st->bg_set ? st->bg : st->under); }
    }
    (void)have_w;
    int px = style ? width : 0;
    if(side < 0){ st->bt = st->br = st->bb = st->bl = px; }
    else set_border_side(st, side, px);
    if(px) st->border_color = color;
}

static void set_display(wstyle_t *st, const char *v){
    st->inline_block = 0; st->grid = 0; st->trow = 0; st->tcell = 0; st->contents = 0;
    if(word_eq(v, "none")) st->display = W_NONE;
    else if(word_eq(v, "block") || word_eq(v, "flow-root") || word_eq(v, "table") || word_eq(v, "table-caption") ||
            word_eq(v, "table-row-group") || word_eq(v, "table-header-group") || word_eq(v, "table-footer-group") ||
            word_eq(v, "block flow") || word_eq(v, "run-in")) st->display = W_BLOCK;
    else if(word_eq(v, "contents")){ st->display = W_INLINE; st->contents = 1; }
    else if(word_eq(v, "inline") || word_eq(v, "ruby")) st->display = W_INLINE;
    else if(word_eq(v, "inline-block") || word_eq(v, "inline-table") || word_eq(v, "inline flow-root")){ st->display = W_INLINE; st->inline_block = 1; }
    else if(word_eq(v, "list-item")) st->display = W_LISTITEM;
    else if(word_eq(v, "flex") || word_eq(v, "-webkit-box") || word_eq(v, "-ms-flexbox") || word_eq(v, "-webkit-flex") || word_eq(v, "block flex")) st->display = W_FLEX;
    else if(word_eq(v, "inline-flex") || word_eq(v, "-webkit-inline-box") || word_eq(v, "-ms-inline-flexbox")){ st->display = W_FLEX; st->inline_block = 1; }
    else if(word_eq(v, "grid") || word_eq(v, "-ms-grid")){ st->display = W_FLEX; st->grid = 1; }
    else if(word_eq(v, "inline-grid")){ st->display = W_FLEX; st->grid = 1; st->inline_block = 1; }
    else if(word_eq(v, "table-row")){ st->display = W_FLEX; st->trow = 1; }
    else if(word_eq(v, "table-cell")){ st->display = W_BLOCK; st->tcell = 1; }
}

/* One track size: > 0 px, < 0 -fr (tenths), 0 auto, GTR_PCT + tenths of a percent. */
static int track_size(const char *w, u32 wl, int base){
    if(wl > 2 && w[wl - 2] == 'f' && w[wl - 1] == 'r'){
        u32 i = 0; int m;
        if(num_milli(w, wl - 2, &i, &m)) return -(m / 100 > 0 ? m / 100 : 1);
        return -10;
    }
    if(starts_n(w, wl, "minmax(")){
        /* minmax(a, b): b decides (fr or a length); auto/min-content take a */
        const char *a = w + 7; u32 al = wl - 8;
        u32 c = 0; int d = 0;
        while(c < al && !(a[c] == ',' && d == 0)){ if(a[c] == '(') d++; if(a[c] == ')') d--; c++; }
        const char *hi = a + c + 1; u32 hl = al > c ? al - c - 1 : 0;
        trim(&hi, &hl);
        int t = track_size(hi, hl, base);
        if(t == 0){ const char *lo = a; u32 ll = c; trim(&lo, &ll); t = track_size(lo, ll, base); if(t < 0) t = 0; }
        return t;
    }
    if(starts_n(w, wl, "fit-content(") || eq_n(w, wl, "auto") || eq_n(w, wl, "max-content") || eq_n(w, wl, "min-content")) return 0;
    len_t l;
    if(eval_len(w, wl, base, &l)){
        if(l.pct && !l.px) return GTR_PCT + l.pct;
        int px = l.px + view_width * l.vw / 1000;
        return px > 0 ? px : 0;
    }
    return 0;
}

/* grid-template-columns: the tracks, or the narrowest column for auto-fill. */
static void grid_columns(wstyle_t *st, const char *v, u32 n){
    st->grid_cols = 0; st->grid_min = 0;
    if(eq_n(v, n, "none") || n == 0) return;
    const char *w[32]; u32 wl[32];
    int cnt = words(v, n, w, wl, 32), cols = 0;
    for(int k = 0; k < cnt && cols < 16; k++){
        if(w[k][0] == '[') continue;                          /* line names */
        if(starts_n(w[k], wl[k], "repeat(")){
            const char *a = w[k] + 7; u32 al = wl[k] - 8;
            u32 c = 0; while(c < al && a[c] != ',') c++;
            const char *r = a + c + 1; u32 rl = al > c ? al - c - 1 : 0;
            trim(&r, &rl);
            if(starts_n(a, c, "auto-fill") || starts_n(a, c, "auto-fit")){
                /* repeat(auto-fill, minmax(200px, 1fr)) or repeat(auto-fit, 250px) */
                int px = 0;
                if(starts_n(r, rl, "minmax(")){
                    const char *m = r + 7; u32 ml = 0; while(m + ml < r + rl && m[ml] != ',') ml++;
                    len_t l;
                    if(eval_len(m, ml, st->size, &l)) px = l.px + (l.pct ? (int)((long long)view_width * l.pct / 1000) : 0);
                } else parse_len(r, rl, st->size, &px);
                st->grid_min = px > 0 ? px : CSSPX(200);
                return;
            }
            int times = 0;
            for(u32 q = 0; q < c; q++) if(c_digit(a[q])) times = times * 10 + (a[q] - '0');
            const char *iw[8]; u32 iwl[8];
            int inner = words(r, rl, iw, iwl, 8);
            for(int t = 0; t < times && cols < 16; t++)
                for(int q = 0; q < inner && cols < 16; q++) if(iw[q][0] != '[') st->gtr[cols++] = track_size(iw[q], iwl[q], st->size);
        } else st->gtr[cols++] = track_size(w[k], wl[k], st->size);
    }
    st->grid_cols = (short)cols;
}

/* A grid line value: "2", "-1", "span 3", "auto", or an area name (*name). */
static void grid_line(const char *v, u32 n, short *line, short *span, const char **name){
    *line = 0; *span = 0;
    trim(&v, &n);
    if(n == 0 || eq_n(v, n, "auto")) return;
    if(starts_n(v, n, "span")){
        int k = 0; for(u32 i = 4; i < n; i++) if(c_digit(v[i])) k = k * 10 + (v[i] - '0');
        *span = (short)(k > 0 ? k : 1);
        return;
    }
    u32 i = 0; int m;
    if(num_milli(v, n, &i, &m) && (i == n || v[i] == ' ')){ *line = (short)(m / 1000); return; }
    if(name) *name = ar_str(&parena, v, n);
}

/* text-decoration: may name several lines */
static void text_dec(wstyle_t *st, const char *v){
    if(strstr(v, "none")){ st->underline = 0; st->strike = 0; }
    if(strstr(v, "underline")) st->underline = 1;
    if(strstr(v, "line-through")) st->strike = 1;
}

/* ================= applying ================= */

#define VBUF 1024

/* Applies one declaration. ps is the parent's style (for inherit), psize its font size. */
static void apply_decl(wstyle_t *st, const wstyle_t *ps, int psize, const decl_t *d){
    char sub[VBUF];
    const char *raw = d->v; u32 rn = d->vl;
    if(d->var){
        int r = subst(d->v, d->vl, sub, sizeof sub, 0);
        if(r < 0) return;
        raw = sub; rn = (u32)r;
        trim(&raw, &rn);
    }
    char v[VBUF]; u32 vl = 0;
    for(u32 i = 0; i < rn && vl + 1 < sizeof v; i++) v[vl++] = (char)c_lower(raw[i]);
    v[vl] = 0;
    if(vl == 0) return;
    int inherit = word_eq(v, "inherit");
    int initial = word_eq(v, "initial") || word_eq(v, "unset") || word_eq(v, "revert") || word_eq(v, "revert-layer");
    int base = st->size;
    u32 c;
    switch(d->id){
    case PR_COLOR:
        if(inherit && ps){ st->color = ps->color; break; }
        if(initial){ st->color = ps ? ps->color : 0; break; }
        if(parse_color(v, vl, &c, st->bg_set ? st->bg : st->under)){
            if(c == COL_INHERIT || c == COL_CURRENT){ if(ps) st->color = ps->color; }
            else if(c != COL_TRANSPARENT) st->color = c;
            else st->color = st->bg_set ? st->bg : st->under;          /* invisible text */
        }
        break;
    case PR_BGCOLOR:
        if(inherit && ps){ st->bg_set = ps->bg_set; st->bg = ps->bg; break; }
        if(initial){ st->bg_set = 0; break; }
        if(parse_color(v, vl, &c, st->under)){
            if(c == COL_TRANSPARENT) st->bg_set = 0;
            else if(c == COL_CURRENT){ st->bg_set = 1; st->bg = st->color; }
            else if(c != COL_INHERIT){ st->bg_set = 1; st->bg = c; }
        }
        break;
    case PR_BG: case PR_BGIMAGE: {
        if(word_eq(v, "none") || initial){ if(d->id == PR_BG) st->bg_set = 0; st->bg_img = 0; break; }
        const char *w[12]; u32 wl[12];
        int cnt = words(v, vl, w, wl, 12);
        int got = 0;
        for(int k = 0; k < cnt; k++){
            if(starts_n(w[k], wl[k], "url(") || starts_n(w[k], wl[k], "image-set(") || starts_n(w[k], wl[k], "-webkit-image-set(")){ st->bg_img = 1; continue; }
            if(starts_n(w[k], wl[k], "linear-gradient(") || starts_n(w[k], wl[k], "radial-gradient(") ||
               starts_n(w[k], wl[k], "conic-gradient(") || starts_n(w[k], wl[k], "repeating-linear-gradient(") ||
               starts_n(w[k], wl[k], "-webkit-linear-gradient(")){
                /* the first colour stop stands in for the gradient */
                const char *a = w[k]; u32 al = wl[k];
                while(al && *a != '('){ a++; al--; }
                if(al >= 2){ a++; al -= 2; }
                const char *gw[16]; u32 gwl[16];
                int gn = words(a, al, gw, gwl, 16);
                for(int q = 0; q < gn && !got; q++){
                    if(parse_color(gw[q], gwl[q], &c, st->under) && c != COL_INHERIT){
                        if(c == COL_TRANSPARENT) continue;
                        if(c == COL_CURRENT) c = st->color;
                        st->bg_set = 1; st->bg = c; got = 1;
                    }
                }
                continue;
            }
            if(d->id == PR_BG && !got && parse_color(w[k], wl[k], &c, st->under)){
                if(c == COL_TRANSPARENT){ st->bg_set = 0; got = 1; }
                else if(c != COL_INHERIT){ st->bg_set = 1; st->bg = c == COL_CURRENT ? st->color : c; got = 1; }
            }
        }
        if(d->id == PR_BG && !got && !st->bg_img) st->bg_set = 0;   /* "background: 0 0" and the like reset it */
        break; }
    case PR_FONTSIZE: {
        int px;
        if(inherit){ st->size = psize; break; }
        if(named_size(v, psize, &px)){ st->size = px; st->hide_text = 0; }
        else if(parse_len(v, vl, psize, &px)){
            if(px > 0){ st->size = px; st->hide_text = 0; }
            else st->hide_text = 1;
        }
        break; }
    case PR_FONT: {
        /* [style] [weight] size[/line-height] family */
        if(word_eq(v, "inherit")) break;
        const char *w[10]; u32 wl[10];
        int cnt = words(v, vl, w, wl, 10);
        for(int k = 0; k < cnt; k++){
            if(eq_n(w[k], wl[k], "italic") || eq_n(w[k], wl[k], "oblique")) st->italic = 1;
            else if(eq_n(w[k], wl[k], "bold") || eq_n(w[k], wl[k], "bolder") || (wl[k] == 3 && w[k][0] >= '6' && w[k][0] <= '9' && w[k][1] == '0')) st->bold = 1;
            else if(eq_n(w[k], wl[k], "normal")) { }
            else {
                u32 sl = 0; while(sl < wl[k] && w[k][sl] != '/') sl++;
                int px; char t[24]; u32 tl = sl < 23 ? sl : 23; memcpy(t, w[k], tl); t[tl] = 0;
                if(named_size(t, psize, &px) || ((c_digit(w[k][0]) || w[k][0] == '.') && parse_len(w[k], sl, psize, &px))){
                    if(px > 0){ st->size = px; st->hide_text = 0; } else st->hide_text = 1;
                    st->italic = 0;
                    for(int q = 0; q < k; q++) if(eq_n(w[q], wl[q], "italic") || eq_n(w[q], wl[q], "oblique")) st->italic = 1;
                    st->bold = 0;
                    for(int q = 0; q < k; q++) if(eq_n(w[q], wl[q], "bold") || eq_n(w[q], wl[q], "bolder") || (wl[q] == 3 && w[q][0] >= '6' && w[q][0] <= '9')) st->bold = 1;
                    break;
                }
            }
        }
        break; }
    case PR_FONTWEIGHT:
        if(inherit && ps){ st->bold = ps->bold; break; }
        if(word_eq(v, "bold") || word_eq(v, "bolder") || (vl == 3 && v[0] >= '6' && v[0] <= '9' && v[1] == '0' && v[2] == '0')) st->bold = 1;
        else if(word_eq(v, "normal") || word_eq(v, "lighter") || (vl == 3 && v[0] >= '1' && v[0] <= '5') || initial) st->bold = 0;
        break;
    case PR_FONTSTYLE:
        st->italic = starts_n(v, vl, "italic") || starts_n(v, vl, "oblique");
        break;
    case PR_WHITESPACE:
        st->white_pre = word_eq(v, "pre") || word_eq(v, "pre-wrap") || word_eq(v, "pre-line") || word_eq(v, "break-spaces");
        break;
    case PR_TEXTDEC: text_dec(st, v); break;
    case PR_TEXTALIGN:
        if(word_eq(v, "center") || word_eq(v, "-webkit-center") || word_eq(v, "-moz-center")) st->align = 1;
        else if(word_eq(v, "right") || word_eq(v, "end") || word_eq(v, "-webkit-right")) st->align = 2;
        else if(word_eq(v, "left") || word_eq(v, "start") || word_eq(v, "justify") || word_eq(v, "-webkit-left") || initial) st->align = 0;
        else if(inherit && ps) st->align = ps->align;
        break;
    case PR_TEXTTRANSFORM:
        st->upper = word_eq(v, "uppercase") ? 1 : word_eq(v, "lowercase") ? 2 : 0;
        break;
    case PR_TEXTINDENT: {
        int px;
        if(parse_len(v, vl, base, &px)) st->hide_text = px < CSSPX(-500) ? 1 : st->hide_text;
        break; }
    case PR_DISPLAY: set_display(st, v); break;
    case PR_WIDTH: {
        len_t l;
        if(word_eq(v, "auto") || word_eq(v, "fit-content") || word_eq(v, "max-content") || word_eq(v, "min-content") || initial){ st->width = st->width_pct = st->width_vw = 0; break; }
        if(word_eq(v, "100%") || word_eq(v, "-webkit-fill-available") || word_eq(v, "stretch") || word_eq(v, "-moz-available")){ st->width = 0; st->width_pct = 1000; st->width_vw = 0; break; }
        if(eval_len(v, vl, base, &l)){ st->width = l.px; st->width_pct = l.pct; st->width_vw = l.vw; }
        break; }
    case PR_MINWIDTH: {
        int px;
        if(parse_len(v, vl, base, &px) && !strchr(v, '%')) st->min_w = px;
        else st->min_w = 0;
        break; }
    case PR_MAXWIDTH: {
        len_t l;
        if(word_eq(v, "none") || initial){ st->max_w = st->max_pct = st->max_vw = 0; }
        else if(eval_len(v, vl, base, &l)){ st->max_w = l.px; st->max_pct = l.pct; st->max_vw = l.vw; if(!l.px && !l.pct && !l.vw) st->max_w = 1; }
        break; }
    case PR_HEIGHT: case PR_MINHEIGHT: {
        int px;
        if(strchr(v, '%')) break;                                     /* % of an unknown height */
        if(parse_len(v, vl, base, &px)){
            if(px > 0){ if(d->id == PR_HEIGHT || px > st->height) st->height = px; if(d->id == PR_HEIGHT) st->zero_h = 0; }
            else if(d->id == PR_HEIGHT){ st->zero_h = 1; st->height = 0; }
        } else if(d->id == PR_HEIGHT && (word_eq(v, "auto") || initial)){ st->height = 0; st->zero_h = 0; }
        break; }
    case PR_MAXHEIGHT: {
        int px;
        st->max_h0 = parse_len(v, vl, base, &px) && px == 0 && !strchr(v, '%');
        break; }
    case PR_LISTSTYLE:
        st->no_bullet = starts_n(v, vl, "none") || strstr(v, " none") != NULL;
        break;
    case PR_FLEX: {
        const char *w[3]; u32 wl[3];
        int cnt = words(v, vl, w, wl, 3);
        if(word_eq(v, "none")){ st->flex_grow = 0; st->no_shrink = 1; break; }
        if(word_eq(v, "auto")){ st->flex_grow = 1; st->no_shrink = 0; st->basis = st->basis_pct = 0; break; }
        if(initial){ st->flex_grow = 0; st->no_shrink = 0; break; }
        int nums = 0, has_auto = 0;
        st->basis = st->basis_pct = 0; st->basis_set = 0;
        for(int k = 0; k < cnt; k++) if(eq_n(w[k], wl[k], "auto") || eq_n(w[k], wl[k], "content")) has_auto = 1;
        for(int k = 0; k < cnt; k++){
            u32 i = 0; int m;
            int pure = num_milli(w[k], wl[k], &i, &m) && i == wl[k];
            if(pure && nums == 0){ st->flex_grow = m >= 1000 ? m / 1000 : (m > 0 ? 1 : 0); nums++; }
            else if(pure && nums == 1){ st->no_shrink = m == 0; nums++; }
            else if(!eq_n(w[k], wl[k], "auto") && !eq_n(w[k], wl[k], "content")){ len_t l; if(eval_len(w[k], wl[k], base, &l)){ st->basis = l.px; st->basis_pct = l.pct; st->basis_set = 1; } }
        }
        if(nums == 0) st->flex_grow = 1;                    /* "flex: 200px" means 1 1 200px */
        if(nums >= 1 && !st->basis_set && !has_auto){ st->basis = 0; st->basis_pct = 0; st->basis_set = 1; }   /* "flex: 1" means 1 1 0% */
        break; }
    case PR_FLEXGROW: {
        u32 i = 0; int m;
        if(num_milli(v, vl, &i, &m)) st->flex_grow = m >= 1000 ? m / 1000 : (m > 0 ? 1 : 0);
        break; }
    case PR_FLEXSHRINK: {
        u32 i = 0; int m;
        if(num_milli(v, vl, &i, &m)) st->no_shrink = m == 0;
        break; }
    case PR_FLEXBASIS: {
        len_t l;
        st->basis = st->basis_pct = 0; st->basis_set = 0;
        if(!word_eq(v, "auto") && !word_eq(v, "content") && eval_len(v, vl, base, &l)){ st->basis = l.px; st->basis_pct = l.pct; st->basis_set = 1; }
        break; }
    case PR_FLEXWRAP: st->flex_wrap = word_eq(v, "wrap") || word_eq(v, "wrap-reverse"); break;
    case PR_FLEXDIR: st->flex_col = starts_n(v, vl, "column"); break;
    case PR_FLEXFLOW:
        st->flex_col = strstr(v, "column") != NULL;
        st->flex_wrap = strstr(v, "wrap") != NULL && strstr(v, "nowrap") == NULL;
        break;
    case PR_JUSTIFY:
        st->justify = word_eq(v, "center") ? 1 :
                      (word_eq(v, "flex-end") || word_eq(v, "end") || word_eq(v, "right")) ? 2 :
                      word_eq(v, "space-between") ? 3 :
                      (word_eq(v, "space-around") || word_eq(v, "space-evenly")) ? 4 : 0;
        break;
    case PR_BOXSIZING: st->border_box = word_eq(v, "border-box"); break;
    case PR_OVERFLOW: st->ovf_hidden = starts_n(v, vl, "hidden") || starts_n(v, vl, "clip"); break;
    case PR_VISIBILITY:
        if(word_eq(v, "hidden") || word_eq(v, "collapse")) st->display = W_NONE;
        break;
    case PR_BORDER: border_short(st, -1, v, vl); break;
    case PR_BORDER_SIDE: case PR_BORDER_SIDE_R: case PR_BORDER_SIDE_B: case PR_BORDER_SIDE_L:
        border_short(st, d->id - PR_BORDER_SIDE, v, vl);
        break;
    case PR_BORDER_W: {
        const char *w[4]; u32 wl[4];
        int cnt = words(v, vl, w, wl, 4), px[4];
        for(int k = 0; k < cnt; k++) if(!border_width_word(w[k], wl[k], base, &px[k])) return;
        if(cnt == 1){ st->bt = st->br = st->bb = st->bl = px[0]; }
        else if(cnt == 2){ st->bt = st->bb = px[0]; st->br = st->bl = px[1]; }
        else if(cnt == 3){ st->bt = px[0]; st->br = st->bl = px[1]; st->bb = px[2]; }
        else if(cnt == 4){ st->bt = px[0]; st->br = px[1]; st->bb = px[2]; st->bl = px[3]; }
        break; }
    case PR_BORDER_W_SIDE: case PR_BORDER_W_R: case PR_BORDER_W_B: case PR_BORDER_W_L: {
        int px;
        if(border_width_word(v, vl, base, &px)) set_border_side(st, d->id - PR_BORDER_W_SIDE, px);
        break; }
    case PR_BORDER_C: case PR_BORDER_C_SIDE: {
        const char *w[4]; u32 wl[4];
        int cnt = words(v, vl, w, wl, 4);
        if(cnt && parse_color(w[cnt - 1], wl[cnt - 1], &c, st->under) && c != COL_INHERIT)
            st->border_color = c == COL_CURRENT ? st->color : c == COL_TRANSPARENT ? (st->bg_set ? st->bg : st->under) : c;
        break; }
    case PR_BORDER_STYLE: {
        const char *w[4]; u32 wl[4];
        int cnt = words(v, vl, w, wl, 4);
        if(cnt == 1 && border_style_word(w[0], wl[0]) == 0) st->bt = st->br = st->bb = st->bl = 0;
        break; }
    case PR_BORDER_S_SIDE: case PR_BORDER_S_R: case PR_BORDER_S_B: case PR_BORDER_S_L:
        if(border_style_word(v, vl) == 0) set_border_side(st, d->id - PR_BORDER_S_SIDE, 0);
        break;
    case PR_GAP: {
        const char *w[2]; u32 wl[2]; int px;
        int cnt = words(v, vl, w, wl, 2);
        if(cnt >= 1 && parse_len(w[0], wl[0], base, &px) && !strchr(v, '%')){ st->rgap = px; st->gap = px; }
        if(cnt == 2 && parse_len(w[1], wl[1], base, &px)) st->gap = px;
        break; }
    case PR_ROWGAP: { int px; if(parse_len(v, vl, base, &px)) st->rgap = px; break; }
    case PR_COLGAP: { int px; if(parse_len(v, vl, base, &px)) st->gap = px; break; }
    case PR_GRIDCOLS: grid_columns(st, v, vl); break;
    case PR_GRIDTEMPLATE: {
        /* "rows / columns", maybe with area strings: "'a b' 1fr 'c d' auto / 200px 1fr" */
        const char *sl = NULL; int q = 0;
        for(const char *p = v; *p; p++){ if(*p == '"' || *p == '\'') q = !q; if(*p == '/' && !q) sl = p; }
        if(sl){ const char *g = sl + 1; u32 gl = (u32)(v + vl - g); trim(&g, &gl); grid_columns(st, g, gl); }
        if(strchr(v, '"') || strchr(v, '\'')) st->gareas = ar_str(&parena, raw, sl ? (u32)(sl - v) : rn);
        break; }
    case PR_GRIDAREAS:
        st->gareas = word_eq(v, "none") ? NULL : ar_str(&parena, raw, rn);
        break;
    case PR_GRIDCOLUMN: case PR_GRIDROW: case PR_GRIDAREA: case PR_GRIDCOLSTART: case PR_GRIDCOLEND: {
        /* "1 / -1", "span 2", "2 / span 3", or an area name */
        const char *sl = strchr(v, '/');
        u32 first = sl ? (u32)(sl - v) : vl;
        const char *name = NULL;
        short ln, sp, ln2 = 0, sp2 = 0;
        grid_line(raw, first, &ln, &sp, &name);
        if(sl) grid_line(raw + first + 1, rn - first - 1, &ln2, &sp2, NULL);
        if(d->id == PR_GRIDAREA){ st->garea = name; st->gcs = ln; st->gcspan = sp; st->grs = 0; break; }
        if(d->id == PR_GRIDCOLSTART){ st->gcs = ln; if(sp) st->gcspan = sp; if(name) st->garea = name; break; }
        if(d->id == PR_GRIDCOLEND){ st->gce = ln; if(sp) st->gcspan = sp; break; }
        if(d->id == PR_GRIDCOLUMN){ st->gcs = ln; st->gce = ln2; st->gcspan = sp ? sp : sp2; if(name) st->garea = name; }
        else { st->grs = ln; st->gre = ln2; st->grspan = sp ? sp : sp2; }
        break; }
    case PR_MARGIN: {
        int autos = 0;
        int m[4] = { st->m[0], st->m[1], st->m[2], st->m[3] };
        parse_box(v, vl, base, m, &autos);
        memcpy(st->m, m, sizeof m);
        st->auto_lr = autos;
        break; }
    case PR_MARGIN_T: case PR_MARGIN_R: case PR_MARGIN_B: case PR_MARGIN_L: {
        int side = d->id - PR_MARGIN_T, px;
        if(word_eq(v, "auto")){ if(side == 1) st->auto_lr |= 1; else if(side == 3) st->auto_lr |= 2; st->m[side] = 0; }
        else if(parse_len(v, vl, base, &px)){ st->m[side] = px; if(side == 1) st->auto_lr &= ~1; if(side == 3) st->auto_lr &= ~2; }
        break; }
    case PR_MARGIN_INLINE: case PR_MARGIN_BLOCK: case PR_PADDING_INLINE: case PR_PADDING_BLOCK: {
        const char *w[2]; u32 wl[2];
        int cnt = words(v, vl, w, wl, 2);
        if(!cnt) break;
        int a = 0, b = 0, aa = eq_n(w[0], wl[0], "auto"), ba;
        if(!aa) parse_len(w[0], wl[0], base, &a);
        if(cnt == 2){ ba = eq_n(w[1], wl[1], "auto"); if(!ba) parse_len(w[1], wl[1], base, &b); } else { b = a; ba = aa; }
        int *box = (d->id == PR_MARGIN_INLINE || d->id == PR_MARGIN_BLOCK) ? st->m : st->p;
        if(d->id == PR_MARGIN_INLINE || d->id == PR_PADDING_INLINE){
            box[3] = a; box[1] = b;
            if(d->id == PR_MARGIN_INLINE) st->auto_lr = (aa ? 2 : 0) | (ba ? 1 : 0);
        } else { box[0] = a; box[2] = b; }
        break; }
    case PR_PADDING: parse_box(v, vl, base, st->p, NULL); for(int k = 0; k < 4; k++) if(st->p[k] < 0) st->p[k] = 0; break;
    case PR_PADDING_T: case PR_PADDING_R: case PR_PADDING_B: case PR_PADDING_L: {
        int px;
        if(parse_len(v, vl, base, &px) && !strchr(v, '%')) st->p[d->id - PR_PADDING_T] = px < 0 ? 0 : px;
        break; }
    case PR_POSITION:
        st->pos = word_eq(v, "relative") ? 1 : word_eq(v, "absolute") ? 2 : word_eq(v, "fixed") ? 3 :
                  (word_eq(v, "sticky") || word_eq(v, "-webkit-sticky")) ? 4 : 0;
        break;
    case PR_FLOAT:
        st->float_ = (word_eq(v, "left") || word_eq(v, "inline-start")) ? 1 : (word_eq(v, "right") || word_eq(v, "inline-end")) ? 2 : 0;
        break;
    case PR_OPACITY: {
        u32 i = 0; int m;
        st->opacity0 = num_milli(v, vl, &i, &m) && m <= 10 && !(i < vl && v[i] == '%' && m > 1000);
        break; }
    case PR_CLIP:
        st->clipped = starts_n(v, vl, "rect(") && (strstr(v, "0 0 0 0") || strstr(v, "0,0,0,0") || strstr(v, "0, 0, 0, 0") ||
                      strstr(v, "1px 1px 1px 1px") || strstr(v, "1px,1px,1px,1px") || strstr(v, "1px, 1px, 1px, 1px") || strstr(v, "0px 0px 0px 0px"));
        break;
    case PR_CLIPPATH:
        st->clipped = strstr(v, "inset(50%") || strstr(v, "inset(100%") || word_eq(v, "circle(0)") || strstr(v, "circle(0 ") ||
                      word_eq(v, "polygon(0 0, 0 0, 0 0, 0 0)") || word_eq(v, "polygon(0 0,0 0,0 0)") || strstr(v, "inset(0 0 99.9%") ||
                      strstr(v, "inset(0px 0px 99.9%") || strstr(v, "polygon(0px 0px, 0px 0px, 0px 0px)");
        break;
    case PR_LEFT: case PR_TOP: case PR_RIGHT: case PR_BOTTOM: case PR_INSET: {
        const char *w[4]; u32 wl[4];
        int cnt = words(v, vl, w, wl, 4);
        int first = d->id == PR_INSET ? 0 : d->id - PR_TOP, sides = d->id == PR_INSET ? 4 : 1;
        static const int map4[4][4] = { {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
        for(int s = first; s < first + sides; s++){
            int k = d->id == PR_INSET ? (cnt ? map4[cnt - 1][s] : 0) : 0;
            st->off_set &= (u8)~(1 << s);
            if(k >= cnt) continue;
            len_t l;
            if(eq_n(w[k], wl[k], "auto")) continue;
            if(!eval_len(w[k], wl[k], base, &l)) continue;
            st->off[s] = l.px + view_width * l.vw / 1000;
            st->off_pct[s] = (short)(l.pct > 30000 ? 30000 : l.pct < -30000 ? -30000 : l.pct);
            st->off_set |= (u8)(1 << s);
            if((s == 1 || s == 3 || s == 0) && st->off[s] <= CSSPX(-999)) st->offscreen = 1;
            else if(s == 3 || s == 0) st->offscreen = 0;
        }
        break; }
    case PR_MASK:
        st->masked = !word_eq(v, "none") && !initial;
        break;
    case PR_TRANSFORM:
        st->scale0 = word_eq(v, "scale(0)") || word_eq(v, "scale(0, 0)") || word_eq(v, "scaley(0)") || word_eq(v, "scalex(0)");
        if(strstr(v, "translate")){
            /* translateX(-9999px), translate(-100vw): moved off the page */
            const char *p = strstr(v, "translate");
            while(*p && *p != '(') p++;
            if(*p){ int px; const char *q = p + 1; u32 ql = 0; while(q[ql] && q[ql] != ',' && q[ql] != ')') ql++;
                if(parse_len(q, ql, base, &px) && px <= CSSPX(-999)) st->offscreen = 1; }
        }
        break;
    case PR_CONTENT_VIS:
        if(word_eq(v, "hidden")) st->display = W_NONE;
        break;
    }
}

/* ---- defaults, like a browser's stylesheet, and the HTML attributes that style ---- */

static u32 link_color = 0x0000EE;

static int legacy_color(const char *v, u32 *out){
    if(!v) return 0;
    u32 n = (u32)strlen(v);
    if(parse_color(v, n, out, 0xFFFFFF) && *out < 0x1000000) return 1;
    /* "ff6600" without the '#' */
    if(n == 6){ char b[8] = "#"; memcpy(b + 1, v, 6); b[7] = 0; return parse_color(b, 7, out, 0xFFFFFF) && *out < 0x1000000; }
    return 0;
}
static int attr_len(const char *v, int *px, int *pct){
    *px = *pct = 0;
    if(!v) return 0;
    int x = 0, any = 0;
    const char *q = v;
    while(*q == ' ') q++;
    while(c_digit(*q)){ x = x * 10 + (*q - '0'); q++; any = 1; }
    if(*q == '.') { q++; while(c_digit(*q)) q++; }
    if(!any) return 0;
    if(*q == '%') *pct = x * 10; else *px = CSSPX(x);
    return 1;
}
static wnode_t *ancestor(wnode_t *n, const char *tag){
    for(wnode_t *p = n->parent; p; p = p->parent) if(!p->text && strcmp(p->tag, tag) == 0) return p;
    return NULL;
}

static void block_margins(wstyle_t *st, int top, int bottom){ st->m[0] = top; st->m[2] = bottom; }

static int is_tag(const char *t, const char *const *list){
    for(int i = 0; list[i]; i++) if(strcmp(t, list[i]) == 0) return 1;
    return 0;
}

static void defaults(wnode_t *n, const wstyle_t *ps){
    wstyle_t *st = &n->st;
    const char *t = n->tag;
    int psize = ps ? ps->size : CSSPX(16);
    static const char *const never_shown[] = { "head", "script", "style", "title", "meta", "link", "noscript", "template",
        "base", "svg", "math", "iframe", "frame", "frameset", "audio", "canvas", "object", "embed", "map",
        "area", "param", "source", "track", "datalist", "slot", "noembed", "noframes", "portal", "dialog", "head", 0 };
    if(n->hidden){ st->display = W_NONE; return; }
    if(is_tag(t, never_shown)){
        if(strcmp(t, "dialog") == 0 && wnode_attr_get(n, "open")){ st->display = W_BLOCK; return; }
        st->display = W_NONE;
        return;
    }
    if(strcmp(t, "input") == 0){
        const char *ty = wnode_attr_get(n, "type");
        if(ty && strcmp(ty, "hidden") == 0){ st->display = W_NONE; return; }
    }
    /* the closed parts of a <details> */
    if(n->parent && strcmp(n->parent->tag, "details") == 0 && !wnode_attr_get(n->parent, "open") && strcmp(t, "summary") != 0){
        st->display = W_NONE; return;
    }
    /* a <select> shows its chosen option */
    if(strcmp(t, "option") == 0 || strcmp(t, "optgroup") == 0){
        wnode_t *sel = ancestor(n, "select");
        int show = 0;
        if(sel && strcmp(t, "option") == 0){
            wnode_t *chosen = NULL, *first = NULL;
            for(wnode_t *p = sel->first; p && !chosen; p = p->next){
                if(p->text) continue;
                wnode_t *list[2] = { p, NULL };
                if(strcmp(p->tag, "optgroup") == 0) list[0] = p->first;
                for(wnode_t *o = list[0]; o; o = (list[0] == p ? NULL : o->next)){
                    if(!o->text && strcmp(o->tag, "option") == 0){
                        if(!first) first = o;
                        if(wnode_attr_get(o, "selected")){ chosen = o; break; }
                    }
                    if(list[0] == p) break;
                }
            }
            show = n == (chosen ? chosen : first);
        }
        st->display = show ? W_INLINE : W_NONE;
        return;
    }
    if(strcmp(t, "optgroup") == 0){ st->display = W_INLINE; return; }

    static const char *const blocks[] = { "html", "body", "div", "section", "article", "main", "header", "footer",
        "nav", "aside", "form", "center", "address", "figure", "figcaption", "#root", "dl", "caption", "details",
        "summary", "fieldset", "legend", "hgroup", "search", "thead", "tbody", "tfoot", "table", "tr", "td", "th",
        "menu", "dir", "frameset", "optgroup", "picture-block", 0 };
    if(is_tag(t, blocks)){
        st->display = W_BLOCK;
        if(strcmp(t, "body") == 0){
            st->m[0] = st->m[1] = st->m[2] = st->m[3] = CSSPX(8);
            u32 c;
            if(legacy_color(wnode_attr_get(n, "text"), &c)) st->color = c;
            if(legacy_color(wnode_attr_get(n, "link"), &c)) link_color = c;
            const char *mw = wnode_attr_get(n, "marginwidth"), *mh = wnode_attr_get(n, "marginheight");
            const char *lm = wnode_attr_get(n, "leftmargin"), *tm = wnode_attr_get(n, "topmargin");
            int px, pct;
            if(attr_len(mw ? mw : lm, &px, &pct)) st->m[1] = st->m[3] = px;
            if(attr_len(mh ? mh : tm, &px, &pct)) st->m[0] = st->m[2] = px;
        }
        else if(strcmp(t, "center") == 0) st->align = 1;
        else if(strcmp(t, "address") == 0) st->italic = 1;
        else if(strcmp(t, "figure") == 0){ block_margins(st, psize, psize); st->m[1] = st->m[3] = CSSPX(40); }
        else if(strcmp(t, "fieldset") == 0){
            st->m[1] = st->m[3] = CSSPX(2);
            st->p[0] = psize * 35 / 100; st->p[2] = psize * 625 / 1000; st->p[1] = st->p[3] = psize * 75 / 100;
            st->bt = st->br = st->bb = st->bl = CSSPX(1); st->border_color = 0xC0C0C0;
        }
        else if(strcmp(t, "summary") == 0 || strcmp(t, "caption") == 0){ if(t[0] == 'c') st->align = 1; }
        else if(strcmp(t, "dl") == 0) block_margins(st, psize, psize);
        else if(strcmp(t, "menu") == 0 || strcmp(t, "dir") == 0){ block_margins(st, psize, psize); st->p[3] = CSSPX(40); }
        else if(strcmp(t, "table") == 0){
            if(web_quirks){ st->size = CSSPX(16); st->bold = 0; st->italic = 0; st->align = 0; st->white_pre = 0; }
            /* a table inside a centred block is centred, like a browser's -webkit-center */
            if(ps && ps->align == 1) st->auto_lr = 3;
        }
        else if(strcmp(t, "td") == 0 || strcmp(t, "th") == 0){
            st->p[0] = st->p[1] = st->p[2] = st->p[3] = CSSPX(1);
            if(t[1] == 'h'){ st->bold = 1; st->align = 1; }
            wnode_t *tb = ancestor(n, "table");
            if(tb){
                int px, pct;
                if(attr_len(wnode_attr_get(tb, "cellpadding"), &px, &pct)) st->p[0] = st->p[1] = st->p[2] = st->p[3] = px;
                const char *b = wnode_attr_get(tb, "border");
                if(b && (b[0] == 0 || (b[0] >= '1' && b[0] <= '9'))){ st->bt = st->br = st->bb = st->bl = CSSPX(1); st->border_color = 0x808080; }
            }
            if(n->parent && !n->parent->text){
                const char *ra = wnode_attr_get(n->parent, "align");
                if(ra){ if(strcmp(ra, "center") == 0) st->align = 1; else if(strcmp(ra, "right") == 0) st->align = 2; }
            }
        }
        goto attrs;
    }
    if(strcmp(t, "p") == 0){ st->display = W_BLOCK; block_margins(st, psize, psize); goto attrs; }
    if(strcmp(t, "dd") == 0){ st->display = W_BLOCK; st->m[3] = CSSPX(40); goto attrs; }
    if(strcmp(t, "dt") == 0){ st->display = W_BLOCK; goto attrs; }
    if(t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0){
        static const int hs[6] = { 200, 150, 117, 100, 83, 67 };
        static const int hm[6] = { 67, 83, 100, 133, 167, 233 };
        int lvl = t[1] - '1';
        st->display = W_BLOCK;
        st->size = psize * hs[lvl] / 100;
        st->bold = 1;
        block_margins(st, st->size * hm[lvl] / 100, st->size * hm[lvl] / 100);
        goto attrs;
    }
    if(strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0){
        st->display = W_BLOCK;
        int nested = n->parent && (strcmp(n->parent->tag, "li") == 0 || strcmp(n->parent->tag, "ul") == 0 || strcmp(n->parent->tag, "ol") == 0 || strcmp(n->parent->tag, "dd") == 0);
        if(!nested) block_margins(st, psize, psize);
        st->p[3] = CSSPX(40);
        goto attrs;
    }
    if(strcmp(t, "li") == 0){ st->display = W_LISTITEM; goto attrs; }
    if(strcmp(t, "blockquote") == 0){
        st->display = W_BLOCK;
        block_margins(st, psize, psize);
        st->m[1] = st->m[3] = CSSPX(40);
        goto attrs;
    }
    if(strcmp(t, "pre") == 0 || strcmp(t, "listing") == 0 || strcmp(t, "xmp") == 0 || strcmp(t, "plaintext") == 0){
        st->display = W_BLOCK;
        block_margins(st, psize, psize);
        st->size = psize * 13 / 16;
        st->white_pre = 1;
        goto attrs;
    }
    if(strcmp(t, "hr") == 0){
        st->display = W_BLOCK;
        block_margins(st, CSSPX(8), CSSPX(8));
        st->bt = CSSPX(1); st->bb = CSSPX(1);
        st->border_color = 0xBFBFBF;
        goto attrs;
    }
    if(strcmp(t, "b") == 0 || strcmp(t, "strong") == 0) st->bold = 1;
    else if(strcmp(t, "i") == 0 || strcmp(t, "em") == 0 || strcmp(t, "cite") == 0 || strcmp(t, "var") == 0 || strcmp(t, "dfn") == 0) st->italic = 1;
    else if(strcmp(t, "u") == 0 || strcmp(t, "ins") == 0) st->underline = 1;
    else if(strcmp(t, "s") == 0 || strcmp(t, "strike") == 0 || strcmp(t, "del") == 0) st->strike = 1;
    else if(strcmp(t, "a") == 0){
        if(n->href){ st->color = link_color; st->underline = 1; }
    }
    else if(strcmp(t, "small") == 0 || strcmp(t, "sub") == 0 || strcmp(t, "sup") == 0) st->size = psize * 5 / 6;
    else if(strcmp(t, "big") == 0) st->size = psize * 6 / 5;
    else if(strcmp(t, "code") == 0 || strcmp(t, "kbd") == 0 || strcmp(t, "samp") == 0 || strcmp(t, "tt") == 0) st->size = psize * 13 / 16;
    else if(strcmp(t, "mark") == 0){ st->bg_set = 1; st->bg = 0xFFFF00; st->color = 0x000000; }
    else if(strcmp(t, "button") == 0){ st->bg_set = 1; st->bg = 0xE9E9ED; st->color = 0x000000; st->inline_block = 1; }
    else if(strcmp(t, "select") == 0){ st->bg_set = 1; st->bg = 0xF0F0F3; st->color = 0x000000; }
    else if(strcmp(t, "textarea") == 0){ st->bg_set = 1; st->bg = 0xFFFFFF; st->white_pre = 1; st->size = psize * 13 / 16;
        st->bt = st->br = st->bb = st->bl = CSSPX(1); st->border_color = 0x767676; st->inline_block = 1; }
    else if(strcmp(t, "font") == 0){
        u32 c;
        if(legacy_color(wnode_attr_get(n, "color"), &c)) st->color = c;
        const char *sz = wnode_attr_get(n, "size");
        if(sz){
            static const int fs[8] = { 0, 10, 13, 16, 18, 24, 32, 48 };
            int rel = sz[0] == '+' || sz[0] == '-', k = 0;
            for(const char *q = sz + rel; c_digit(*q); q++) k = k * 10 + (*q - '0');
            if(rel) k = sz[0] == '+' ? 3 + k : 3 - k;
            if(k < 1) k = 1; if(k > 7) k = 7;
            st->size = CSSPX(fs[k]);
        }
    }
    else if(strcmp(t, "img") == 0){
        int px, pct;
        if(attr_len(wnode_attr_get(n, "width"), &px, &pct)){ st->width = px; st->width_pct = pct; }
        if(attr_len(wnode_attr_get(n, "height"), &px, &pct)) st->height = px;
        const char *al = wnode_attr_get(n, "align");
        if(al){ if(strcmp(al, "left") == 0) st->float_ = 1; else if(strcmp(al, "right") == 0) st->float_ = 2; }
    }
    if(st->display == W_NONE) st->display = W_INLINE;
    return;

attrs:
    /* presentational attributes of block elements: bgcolor, align, width */
    {
        u32 c;
        if(legacy_color(wnode_attr_get(n, "bgcolor"), &c)){ st->bg_set = 1; st->bg = c; }
        const char *al = wnode_attr_get(n, "align");
        if(al){
            if(strcmp(t, "table") == 0){ if(strcmp(al, "center") == 0) st->auto_lr = 3; else if(strcmp(al, "right") == 0) st->float_ = 2; else if(strcmp(al, "left") == 0) st->float_ = 1; }
            else if(strcmp(al, "center") == 0 || strcmp(al, "middle") == 0) st->align = 1;
            else if(strcmp(al, "right") == 0) st->align = 2;
            else if(strcmp(al, "left") == 0 || strcmp(al, "justify") == 0) st->align = 0;
        }
        int px, pct;
        if((strcmp(t, "table") == 0 || strcmp(t, "td") == 0 || strcmp(t, "th") == 0 || strcmp(t, "hr") == 0 || strcmp(t, "col") == 0) &&
           attr_len(wnode_attr_get(n, "width"), &px, &pct)){ st->width = px; st->width_pct = pct; if(t[0] == 't' && t[1] == 'a' && st->border_box == 0) st->border_box = 1; }
        if((strcmp(t, "td") == 0 || strcmp(t, "th") == 0 || strcmp(t, "tr") == 0) && attr_len(wnode_attr_get(n, "height"), &px, &pct)) st->height = px;
        if(strcmp(t, "table") == 0){
            const char *b = wnode_attr_get(n, "border");
            if(b && b[0] >= '1' && b[0] <= '9'){ st->bt = st->br = st->bb = st->bl = CSSPX(b[0] - '0'); st->border_color = 0x808080; }
        }
    }
}

/* ================= the cascade ================= */

#define MATCH_MAX 2048
static int mbuf[MATCH_MAX];

static void collect(const wnode_t *n, int *nm){
    /* candidates: universal rules + the buckets of the element's tag, id and classes */
    int seen_b[24]; int nseen = 0;
    #define ADD_BUCKET(b) do { \
        int _b = (int)(b), dup = 0; \
        for(int q = 0; q < nseen; q++) if(seen_b[q] == _b){ dup = 1; break; } \
        if(!dup && nseen < 24){ seen_b[nseen++] = _b; \
            for(int r = bucket_start[_b]; r < bucket_start[_b + 1] && *nm < MATCH_MAX; r++){ \
                const rule_t *ru = &rules[bucket_rules[r]]; \
                if(ru->media && !medias[ru->media].ok) continue; \
                if(sel_match(n, &ru->s)) mbuf[(*nm)++] = bucket_rules[r]; } } } while(0)
    for(int i = 0; i < nuniv && *nm < MATCH_MAX; i++){
        const rule_t *ru = &rules[univ_rules[i]];
        if(ru->media && !medias[ru->media].ok) continue;
        if(sel_match(n, &ru->s)) mbuf[(*nm)++] = univ_rules[i];
    }
    ADD_BUCKET(hash_n(n->tag, (u32)strlen(n->tag)) & (BUCKETS - 1));
    if(n->id) ADD_BUCKET(hash_n(n->id, (u32)strlen(n->id)) & (BUCKETS - 1));
    if(n->cls){
        for(const char *p = n->cls; *p; ){
            while(c_ws(*p)) p++;
            const char *b = p;
            while(*p && !c_ws(*p)) p++;
            if(p > b) ADD_BUCKET(hash_n(b, (u32)(p - b)) & (BUCKETS - 1));
        }
    }
    #undef ADD_BUCKET
}

/* A copy of a sheet without its comments (a comment can hold a quote or a brace, which would
 * otherwise throw the parser off: a comment saying "it's" opens a string). Strings are kept. */
static void add_sheet_clean(const char *t, u32 len){
    char *o = kmalloc(len + 1);
    if(!o){ css_add_sheet(t, len); return; }
    u32 k = 0; char q = 0;
    for(u32 i = 0; i < len; ){
        char c = t[i];
        if(q){
            o[k++] = c; i++;
            if(c == '\\' && i < len){ o[k++] = t[i++]; continue; }
            if(c == q || c == '\n') q = 0;
            continue;
        }
        if(c == '/' && i + 1 < len && t[i + 1] == '*'){
            i += 2;
            while(i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i += 2;
            o[k++] = ' ';
            continue;
        }
        if(c == '"' || c == '\'') q = c;
        o[k++] = c; i++;
    }
    o[k] = 0;
    css_add_sheet(o, k);
    kfree(o);
}

static u32 built_sheets = 0xFFFFFFFFu;
static u8 sheet_parsed[64];
static int inline_parsed = 0;
static int adblock_on = 0, adblock_parsed = 0;
void web_set_adblock(int on){ if(on != adblock_on){ adblock_on = on; web_css_gen++; } }
/* What ad boxes look like on most sites (from the common filter lists): hidden. */
static const char adblock_css[] = ".adsbygoogle,ins.adsbygoogle,amp-ad,amp-embed,[id^=\"google_ads_\"],[id^=\"div-gpt-ad\"],[id^=\"gpt-ad\"],[data-ad-slot],[data-ad-unit],[data-ad-client],[data-google-query-id],[data-adunit],[aria-label=\"Advertisement\"],.ad-slot,.ad-banner,.ad-container,.ad-wrapper,.ad-unit,.adunit,.advertisement,.advert,.adbox,.ads-container,.sponsored-ad,.dfp-ad,.gpt-ad,.taboola,[id^=\"taboola-\"],.trc_related_container,.OUTBRAIN,[id^=\"outbrain_widget\"],[class^=\"ad-slot\"],[id^=\"ad-slot\"],[id^=\"adslot\"],.top-ad,.leaderboard-ad,.sidebar-ad{display:none!important}";

void css_prepare(int width){
    view_width = width;
    if(!built_any || built_gen != web_css_gen){
        /* a new page (or new <style> text): start over */
        css_reset();
        cur_arena = &rarena;
        for(int i = 0; i < 64; i++) sheet_parsed[i] = 0;
        inline_parsed = 0;
        adblock_parsed = 0;
        built_gen = web_css_gen;
        built_any = 1;
        built_sheets = 0xFFFFFFFFu;
    }
    if(built_sheets != web_sheets_gen || !inline_parsed){
        /* only sheets that came since the last time are parsed: each on its own, so one that
           was cut off cannot swallow the next, and in any arrival order (their rules carry
           their place in the page) */
        int added = 0, n = web_sheet_slots();
        for(int i = 0; i < n && i < 63; i++){
            u32 len; const char *t = web_sheet_text(i, &len);
            if(!t || sheet_parsed[i]) continue;
            order_base = (i + 1) << 20;
            add_sheet_clean(t, len);
            sheet_parsed[i] = 1;
            added = 1;
        }
        if(!inline_parsed){
            order_base = 63 << 20;                        /* the page's own <style> comes last */
            if(web_css) add_sheet_clean(web_css, web_css_len);
            inline_parsed = 1;
            added = 1;
        }
        if(adblock_on && !adblock_parsed){
            order_base = 64 << 20;                        /* after the page: its !important wins */
            add_sheet_clean(adblock_css, sizeof adblock_css - 1);
            adblock_parsed = 1;
            added = 1;
        }
        order_base = 0;
        if(added) build_index();
        built_sheets = web_sheets_gen;
    }
    for(int i = 1; i < nmedia; i++){
        media_t *m = &medias[i];
        m->ok = mq_eval(m->q, m->qn) && medias[m->parent].ok;     /* parents come first */
    }
    var_reset();
    ar_free(&parena);
    link_color = 0x0000EE;
    root_size = CSSPX(16);
}
void css_set_viewport(int w){ view_width = w; }

/* The window the page is shown in, in device px: @media and scripts see it before the first
 * layout (scripts run as the page loads). */
void web_set_viewport(int w, int h){
    if(w > 0) view_width = w;
    if(h > 0) view_height = h;
}
int css_viewport_w(void){ return view_width * DPR_DEN / DPR_NUM; }
int css_viewport_h(void){ return view_height * DPR_DEN / DPR_NUM; }
/* window.matchMedia(q).matches */
int css_media_matches(const char *q){ return mq_eval(q, (u32)strlen(q)); }

/* Computes n->st from its parent's style, the defaults and the page's rules. */
void css_apply(wnode_t *n, wnode_t *parent){
    wstyle_t *st = &n->st;
    const wstyle_t *ps = parent ? &parent->st : NULL;
    int psize = CSSPX(16);
    if(parent){
        *st = parent->st;
        psize = parent->st.size;
        /* the colour behind this box: the parent's background, or what is behind the parent */
        st->under = ps->bg_set ? ps->bg : ps->under;
        /* properties that are not inherited */
        st->bg_set = 0; st->bg_img = 0;
        memset(st->m, 0, sizeof st->m);
        memset(st->p, 0, sizeof st->p);
        st->underline = 0; st->strike = 0;
        st->display = W_INLINE;
        st->width = 0; st->height = 0; st->border = 0; st->bt = st->br = st->bb = st->bl = 0; st->gap = 0; st->rgap = 0;
        st->width_pct = st->width_vw = 0;
        st->max_w = st->max_pct = st->max_vw = 0; st->min_w = 0;
        st->auto_lr = 0; st->border_box = 0;
        st->flex_grow = st->flex_wrap = st->flex_col = st->justify = 0;
        st->basis = st->basis_pct = 0; st->no_shrink = 0; st->basis_set = 0;
        st->grid = 0; st->grid_cols = 0; st->grid_min = 0;
        st->gareas = NULL; st->garea = NULL; st->gcs = st->gce = st->grs = st->gre = 0; st->gcspan = st->grspan = 0;
        st->inline_block = 0; st->float_ = 0; st->pos = 0; st->trow = 0; st->tcell = 0; st->contents = 0;
        st->opacity0 = st->clipped = st->offscreen = st->scale0 = 0;
        st->masked = 0; st->off_set = 0;
        st->zero_h = st->ovf_hidden = 0; st->max_h0 = 0;
        st->border_color = st->color;
    } else {
        memset(st, 0, sizeof *st);
        st->color = 0x000000;
        st->under = 0xFFFFFF;
        st->size = CSSPX(16);
        st->display = W_BLOCK;
    }
    if(n->text) return;
    defaults(n, ps);
    if(st->display == W_NONE && n->force_show) st->display = W_BLOCK;
    if(st->display == W_NONE) return;

    int nm = 0;
    n->sig = node_sig(n);
    use_sig = 1;
    if(nrules && bucket_start) collect(n, &nm);
    use_sig = 0;
    /* sort by (specificity, order) */
    for(int i = 1; i < nm; i++){
        int v = mbuf[i], j = i - 1;
        while(j >= 0){
            const rule_t *a = &rules[mbuf[j]], *b = &rules[v];
            if(a->s.spec < b->s.spec || (a->s.spec == b->s.spec && a->order <= b->order)) break;
            mbuf[j + 1] = mbuf[j];
            j--;
        }
        mbuf[j + 1] = v;
    }
#ifdef WEB_HOST_DEBUG
    {   /* CSS_TRACE=tag or #id or .class: the rules that matched, in cascade order */
        extern char *getenv(const char *); extern int printf(const char *, ...);
        const char *tr = getenv("CSS_TRACE");
        if(tr && ((tr[0] == '#' && n->id && strcmp(n->id, tr + 1) == 0) || (tr[0] == '.' && has_class(n->cls, tr + 1)) || strcmp(n->tag, tr) == 0)){
            printf("== <%s id=%s class=%s>\n", n->tag, n->id ? n->id : "", n->cls ? n->cls : "");
            for(wattr_t *at = n->attrs; at; at = at->next) printf("   @%s=%s\n", at->name, at->val ? at->val : "");
            for(int i = 0; i < nm; i++){
                const rule_t *r = &rules[mbuf[i]];
                printf("  spec %d media %d [%.*s]:", r->s.spec, r->media, r->media ? (int)(medias[r->media].qn > 70 ? 70 : medias[r->media].qn) : 0, r->media ? medias[r->media].q : "");
                for(int k = 0; k < r->nd && k < 6; k++) printf(" %.*s:%.*s;", r->d[k].nl, r->d[k].name, r->d[k].vl > 60 ? 60 : (int)r->d[k].vl, r->d[k].v);
                printf("\n");
            }
        }
    }
#endif
    decl_t inl[96];
    int ninl = n->style ? split_decls(n->style, (u32)strlen(n->style), inl, 96) : 0;

    /* pass 0: custom properties; pass 1: the font size (other lengths use it); pass 2: the rest.
       In each pass normal declarations go first, then !important ones. */
    for(int pass = 0; pass < 3; pass++){
        for(int imp = 0; imp < 2; imp++){
            for(int i = 0; i < nm; i++){
                const rule_t *r = &rules[mbuf[i]];
                for(int k = 0; k < r->nd; k++){
                    const decl_t *d = &r->d[k];
                    if(d->imp != imp) continue;
                    int is_font = d->id == PR_FONTSIZE || d->id == PR_FONT;
                    if(pass == 0){ if(d->id == PR_CUSTOM) var_set(d->name, d->nl, d->v, d->vl); continue; }
                    if(d->id == PR_CUSTOM || (pass == 1) != is_font) continue;
                    apply_decl(st, ps, psize, d);
                }
            }
            for(int k = 0; k < ninl; k++){
                const decl_t *d = &inl[k];
                if(d->imp != imp) continue;
                int is_font = d->id == PR_FONTSIZE || d->id == PR_FONT;
                if(pass == 0){ if(d->id == PR_CUSTOM) var_set(d->name, d->nl, d->v, d->vl); continue; }
                if(d->id == PR_CUSTOM || (pass == 1) != is_font) continue;
                apply_decl(st, ps, psize, d);
            }
        }
        if(pass == 1 && strcmp(n->tag, "html") == 0) root_size = st->size;
    }

    /* ways pages hide things: off-screen, clipped to nothing, scaled to nothing, collapsed */
    int out_of_flow = st->pos == 2 || st->pos == 3;
    int tiny = st->width > 0 && st->width <= CSSPX(2) && (st->height <= CSSPX(2)) && st->width_pct == 0;
    if(out_of_flow && (st->clipped || st->offscreen || st->opacity0 || (tiny && st->ovf_hidden))) st->display = W_NONE;
    if(st->clipped && (tiny || st->ovf_hidden)) st->display = W_NONE;
    if(st->scale0) st->display = W_NONE;
    if((st->zero_h || st->max_h0) && st->ovf_hidden) st->display = W_NONE;
    if(st->opacity0 && strcmp(n->tag, "input") == 0) st->display = W_NONE;
    if(st->hide_text && st->width > 0 && st->width <= CSSPX(2)) st->display = W_NONE;
    if(n->hidden) st->display = W_NONE;
    if(n->force_show && st->display == W_NONE) st->display = W_BLOCK;
    if(st->masked){ st->bg_set = 0; st->bg_img = 0; }         /* a masked icon: its shape cannot be drawn */
    /* a background picture we cannot draw: keep light text readable on a dark stand-in */
    if(st->bg_img && !st->bg_set){
        u32 c = st->color;
        int lum = (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
        int ulum = (int)(((st->under >> 16) & 255) * 3 + ((st->under >> 8) & 255) * 6 + (st->under & 255)) / 10;
        if(lum > 170 && ulum > 170){ st->bg_set = 1; st->bg = 0x3A4048; }
    }
    if(st->size < 1) st->size = 1;
}

int css_tag_is_hidden(const char *tag){
    return strcmp(tag, "head") == 0 || strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}
