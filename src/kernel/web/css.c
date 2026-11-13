/* CSS: a practical subset. Selectors: tag, #id, .class (two per compound), the
 * descendant, child (>), next-sibling (+) and sibling (~) combinators, :first-child,
 * :last-child, :not(simple), :root, :link, comma lists; @media width queries.
 * Properties: colours, fonts, text-align, display, box sizes, borders, flex layout. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "string.h"

#define RULES_MAX 4096
#define SEL_PARTS 5

enum { P_FIRST = 1, P_LAST = 2, P_EMPTY = 4 };
typedef struct {
    char tag[12]; char id[32]; char cls[32]; char cls2[32];
    int never;                /* uses a feature not supported, or a state never true here (:hover) */
    char comb;                /* how it relates to the compound on its left: ' ', '>', '+' or '~' */
    int pseudo;               /* P_FIRST, P_LAST, P_EMPTY */
    char ntag[12], ncls[32], nid[32];   /* :not(...) of one simple selector */
    int not_never;            /* :not(:checked) and the like: the inner part never matches */
    u64 need;                 /* signature bits an element must have to match (a quick test) */
} compound_t;

/* One bit per name: an element's signature has the bits of its tag, id and classes. */
static u64 name_bit(const char *s, u32 n){
    u32 h = 2166136261u;
    for(u32 i = 0; i < n; i++){ h ^= (unsigned char)s[i]; h *= 16777619u; }
    return (u64)1 << (h & 63);
}
static u64 str_bit(const char *s){ return name_bit(s, (u32)strlen(s)); }
static u64 node_sig(const wnode_t *n){
    u64 m = str_bit(n->tag);
    if(n->id) m |= str_bit(n->id);
    if(n->cls){
        for(const char *p = n->cls; *p; ){
            while(*p == ' ' || *p == '\t' || *p == '\n') p++;
            const char *b = p;
            while(*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if(p > b) m |= name_bit(b, (u32)(p - b));
        }
    }
    return m;
}
static int use_sig = 0;          /* 1 while styling the tree top-down: the signatures are fresh */
typedef struct {
    compound_t c[SEL_PARTS];
    int n;                    /* compounds in the selector */
    int spec;                 /* specificity */
    int order;                /* document order */
    char *decl;               /* the declarations text */
    int min_w, max_w;         /* @media (min-width) / (max-width) in px; 0 = no limit */
} rule_t;

static rule_t rules[RULES_MAX];
static int nrules = 0;
static int view_width = 1024;        /* the page width, for @media */
static int cur_min_w = 0, cur_max_w = 0;   /* the @media block being read */

void css_set_viewport(int w){ view_width = w; }

void css_reset(void){
    for(int i = 0; i < nrules; i++) if(rules[i].decl){ kfree(rules[i].decl); rules[i].decl = NULL; }
    nrules = 0;
}

/* ---- small helpers ---- */
static int c_lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int c_ws(int c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void trim(const char **s, u32 *n){
    while(*n && c_ws((*s)[0])){ (*s)++; (*n)--; }
    while(*n && c_ws((*s)[*n - 1])) (*n)--;
}

static char *copy_n(const char *s, u32 n){
    char *d = kmalloc(n + 1);
    if(!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* ---- parsing rules ---- */

/* Parses one selector ("div.note > p#x") into compounds. Returns 0 if it is usable. */
static int sel_stop(char ch){ return ch == '#' || ch == '.' || ch == ':' || ch == '[' || c_ws(ch) || ch == '>' || ch == '+' || ch == '~' || ch == ')' || ch == '('; }

static void copy_name(const char *s, u32 n, u32 *i, char *dst, u32 cap, int lower_it){
    u32 k = 0;
    while(*i < n && !sel_stop(s[*i])){ if(k + 1 < cap) dst[k++] = lower_it ? (char)c_lower(s[*i]) : s[*i]; (*i)++; }
    dst[k] = 0;
}

/* :not(x) with one simple selector x (a tag, .class, #id or a state like :checked). */
static void parse_not(const char *s, u32 n, compound_t *c){
    u32 i = 0;
    while(i < n && c_ws(s[i])) i++;
    if(i >= n){ return; }
    if(s[i] == '.'){ i++; copy_name(s, n, &i, c->ncls, sizeof c->ncls, 0); }
    else if(s[i] == '#'){ i++; copy_name(s, n, &i, c->nid, sizeof c->nid, 0); }
    else if(s[i] == ':'){ c->not_never = 1; }          /* :not(:checked), :not(:hover)...: always true here */
    else if(s[i] == '['){ c->never = 1; }              /* attribute: not supported */
    else copy_name(s, n, &i, c->ntag, sizeof c->ntag, 1);
    while(i < n && c_ws(s[i])) i++;
    if(i < n) c->never = 1;                            /* more than one simple selector inside */
}

static int parse_selector(const char *s, u32 n, rule_t *r){
    r->n = 0;
    r->spec = 0;
    u32 i = 0;
    char comb = ' ';
    while(i < n){
        while(i < n && c_ws(s[i])) i++;
        if(i >= n) break;
        if(s[i] == '>' || s[i] == '+' || s[i] == '~'){ comb = s[i]; i++; continue; }
        if(r->n >= SEL_PARTS) return -1;
        compound_t *c = &r->c[r->n];
        memset(c, 0, sizeof *c);
        c->comb = r->n ? comb : ' ';
        comb = ' ';
        while(i < n && !c_ws(s[i]) && s[i] != '>' && s[i] != '+' && s[i] != '~'){
            if(s[i] == '#'){
                i++;
                copy_name(s, n, &i, c->id, sizeof c->id, 0);
                r->spec += 100;
            } else if(s[i] == '.'){
                i++;
                char *dst = c->cls[0] ? c->cls2 : c->cls;
                char extra[32];
                if(c->cls[0] && c->cls2[0]) dst = extra;        /* a third class is ignored */
                copy_name(s, n, &i, dst, 32, 0);
                r->spec += 10;
            } else if(s[i] == ':'){
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
                #define PIS(lit) (pl == sizeof(lit) - 1 && memcmp(pn, lit, pl) == 0)
                if(element) c->never = 1;                  /* ::before, ::after, ::marker...: no boxes for them */
                else if(PIS("first-child")) c->pseudo |= P_FIRST;
                else if(PIS("last-child")) c->pseudo |= P_LAST;
                else if(PIS("only-child")) c->pseudo |= P_FIRST | P_LAST;
                else if(PIS("empty")) c->pseudo |= P_EMPTY;
                else if(PIS("root")) memcpy(c->tag, "html", 5);
                else if(PIS("link") || PIS("visited") || PIS("any-link")) { }
                else if(PIS("not") && ae > as) parse_not(s + as, ae - as, c);
                else if(PIS("is") || PIS("where")){
                    /* only a single simple selector inside is understood */
                    u32 j = as;
                    while(j < ae && c_ws(s[j])) j++;
                    if(j < ae && s[j] == '.'){ j++; char *dst = c->cls[0] ? c->cls2 : c->cls; copy_name(s, ae, &j, dst, 32, 0); }
                    else c->never = 1;
                    while(j < ae && c_ws(s[j])) j++;
                    if(j < ae) c->never = 1;
                }
                else c->never = 1;                         /* :hover, :focus, :checked, :nth-child()...: not here */
                #undef PIS
                r->spec += 10;
            } else if(s[i] == '['){
                while(i < n && s[i] != ']') i++;
                if(i < n) i++;
                c->never = 1;
            } else if(s[i] == '*'){
                i++;
            } else if(s[i] == '(' || s[i] == ')'){
                c->never = 1;
                i++;
            } else {
                copy_name(s, n, &i, c->tag, sizeof c->tag, 1);
                r->spec += 1;
            }
        }
        c->need = (c->tag[0] ? str_bit(c->tag) : 0) | (c->id[0] ? str_bit(c->id) : 0) |
                  (c->cls[0] ? str_bit(c->cls) : 0) | (c->cls2[0] ? str_bit(c->cls2) : 0);
        r->n++;
    }
    return r->n > 0 ? 0 : -1;
}

static void add_rule(const char *sel, u32 sn, const char *decl, u32 dn){
    u32 start = 0;
    int depth = 0;
    for(u32 i = 0; i <= sn; i++){
        if(i < sn && sel[i] == '(') depth++;
        if(i < sn && sel[i] == ')') depth--;
        if(i < sn && (sel[i] != ',' || depth > 0)) continue;
        rule_t r;
        memset(&r, 0, sizeof r);
        if(parse_selector(sel + start, i - start, &r) == 0 && nrules < RULES_MAX){
            r.order = nrules;
            r.min_w = cur_min_w;
            r.max_w = cur_max_w;
            r.decl = copy_n(decl, dn);
            rules[nrules++] = r;
        }
        start = i + 1;
    }
}

/* A width in an @media condition: "768px" or "48em" (16 px each). */
static int media_len(const char *p, const char *end){
    while(p < end && (*p == ' ' || *p == ':')) p++;
    int v = 0, any = 0;
    while(p < end && *p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; any = 1; }
    if(p < end && *p == '.'){ p++; while(p < end && *p >= '0' && *p <= '9') p++; }
    if(!any) return 0;
    if(p + 1 < end && (p[0] == 'e' || p[0] == 'r') && (p[1] == 'm' || p[1] == 'e')) v *= 16;
    return v;
}

/* Reads an @media prelude. Returns 0 if its rules can never apply here (print, or "not"). */
static int media_query(const char *p, const char *end, int *min_w, int *max_w){
    *min_w = 0; *max_w = 0;
    int screen = 0, print = 0;
    for(const char *q = p; q < end; q++){
        u32 left = (u32)(end - q);
        if(left >= 5 && memcmp(q, "print", 5) == 0) print = 1;
        if(left >= 6 && memcmp(q, "screen", 6) == 0) screen = 1;
        if(left >= 3 && memcmp(q, "all", 3) == 0) screen = 1;
        if(left >= 4 && memcmp(q, "not ", 4) == 0) return 0;
        if(left >= 9 && memcmp(q, "min-width", 9) == 0) *min_w = media_len(q + 9, end);
        if(left >= 9 && memcmp(q, "max-width", 9) == 0) *max_w = media_len(q + 9, end);
        /* height, orientation, prefers-*, hover...: treated as true */
    }
    if(print && !screen) return 0;
    return 1;
}

/* The index just past the block that starts at the '{' at i. */
static u32 skip_block(const char *t, u32 len, u32 i){
    int depth = 0;
    while(i < len){
        if(t[i] == '{') depth++;
        if(t[i] == '}'){ depth--; if(depth == 0) return i + 1; }
        i++;
    }
    return len;
}

void css_add_sheet(const char *t, u32 len){
    u32 i = 0;
    while(i < len){
        /* comments */
        if(i + 1 < len && t[i] == '/' && t[i+1] == '*'){
            i += 2;
            while(i + 1 < len && !(t[i] == '*' && t[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if(c_ws(t[i]) || t[i] == '<' || t[i] == '>' || t[i] == '}'){ i++; continue; }
        /* at-rules: @media and @supports/@layer blocks hold rules; the others are skipped */
        if(t[i] == '@'){
            u32 a = i;
            while(i < len && t[i] != ';' && t[i] != '{') i++;
            if(i >= len) break;
            if(t[i] == ';'){ i++; continue; }          /* @import, @charset... */
            u32 open = i;
            u32 close = skip_block(t, len, i);
            int is_media = open - a >= 6 && memcmp(t + a, "@media", 6) == 0;
            int is_group = (open - a >= 9 && memcmp(t + a, "@supports", 9) == 0) ||
                           (open - a >= 6 && memcmp(t + a, "@layer", 6) == 0);
            if(is_media && cur_min_w == 0 && cur_max_w == 0){
                int mn, mx;
                if(media_query(t + a + 6, t + open, &mn, &mx)){
                    cur_min_w = mn; cur_max_w = mx;
                    css_add_sheet(t + open + 1, close - open - 2);
                    cur_min_w = 0; cur_max_w = 0;
                }
            } else if(is_group){
                css_add_sheet(t + open + 1, close - open - 2);
            }
            i = close;
            continue;
        }
        u32 ss = i;
        while(i < len && t[i] != '{') i++;
        u32 se = i;
        if(i >= len) break;
        i++;                               /* the '{' */
        u32 ds = i;
        while(i < len && t[i] != '}') i++;
        u32 de = i;
        if(i < len) i++;                   /* the '}' */
        const char *sp = t + ss; u32 sn = se - ss;
        trim(&sp, &sn);
        if(sn) add_rule(sp, sn, t + ds, de - ds);
    }
}

/* ---- matching ---- */

static int has_class(const char *list, const char *cls){
    /* list is space-separated; cls is one name */
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

static const wnode_t *prev_elem(const wnode_t *n){
    if(!n->parent) return NULL;
    const wnode_t *prev = NULL;
    for(const wnode_t *c = n->parent->first; c && c != n; c = c->next) if(!c->text) prev = c;
    return prev;
}
static const wnode_t *next_elem(const wnode_t *n){
    for(const wnode_t *c = n->next; c; c = c->next) if(!c->text) return c;
    return NULL;
}

static int compound_match(const wnode_t *n, const compound_t *c){
    if(n->text) return 0;
    if(c->never) return 0;
    if(use_sig && (n->sig & c->need) != c->need) return 0;
    if(c->tag[0] && strcmp(c->tag, n->tag) != 0) return 0;
    if(c->id[0] && (!n->id || strcmp(c->id, n->id) != 0)) return 0;
    if(c->cls[0] && !has_class(n->cls, c->cls)) return 0;
    if(c->cls2[0] && !has_class(n->cls, c->cls2)) return 0;
    if(c->pseudo){
        if((c->pseudo & P_FIRST) && prev_elem(n)) return 0;
        if((c->pseudo & P_LAST) && next_elem(n)) return 0;
        if((c->pseudo & P_EMPTY) && n->first) return 0;
    }
    if(!c->not_never){
        /* :not(x): fails when x matches */
        int inner = c->ntag[0] || c->ncls[0] || c->nid[0];
        if(inner){
            int m = (!c->ntag[0] || strcmp(c->ntag, n->tag) == 0) &&
                    (!c->ncls[0] || has_class(n->cls, c->ncls)) &&
                    (!c->nid[0] || (n->id && strcmp(c->nid, n->id) == 0));
            if(m) return 0;
        }
    }
    return 1;
}

/* Does n match compound k and, through the combinators, everything to its left? */
static int match_from(const wnode_t *n, const rule_t *r, int k){
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

static int selector_match(const wnode_t *n, const rule_t *r){
    return r->n > 0 && match_from(n, r, r->n - 1);
}

/* Does element n match the selector list sel ("div.a, #b p")? For querySelector. */
int css_matches(const wnode_t *n, const char *sel){
    u32 len = (u32)strlen(sel), start = 0;
    int depth = 0;
    for(u32 i = 0; i <= len; i++){
        if(i < len && sel[i] == '(') depth++;
        if(i < len && sel[i] == ')') depth--;
        if(i < len && (sel[i] != ',' || depth > 0)) continue;
        rule_t r;
        memset(&r, 0, sizeof r);
        const char *sp = sel + start; u32 sn = i - start;
        trim(&sp, &sn);
        if(sn && parse_selector(sp, sn, &r) == 0 && selector_match(n, &r)) return 1;
        start = i + 1;
    }
    return 0;
}

/* ---- values ---- */

static int hexv(int c){
    if(c >= '0' && c <= '9') return c - '0';
    c = c_lower(c);
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

typedef struct { const char *name; u32 rgb; } named_t;
static const named_t colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "navy", 0x000080 }, { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "purple", 0x800080 },
    { "teal", 0x008080 }, { "maroon", 0x800000 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF },
    { "fuchsia", 0xFF00FF }, { "olive", 0x808000 }, { "brown", 0xA52A2A }, { "pink", 0xFFC0CB },
    { "gold", 0xFFD700 }, { "transparent", 0xFFFFFFFF }, { "inherit", 0xFFFFFFFE },
};

/* Parses a colour. Returns 1 on success; 0xFFFFFFFF means transparent (keep the old one). */
static int parse_color(const char *v, u32 n, u32 *out){
    trim(&v, &n);
    if(n && v[0] == '#'){
        u32 rgb = 0; int digits = 0;
        for(u32 i = 1; i < n; i++){
            int h = hexv(v[i]);
            if(h < 0) return 0;
            rgb = rgb * 16 + (u32)h;
            digits++;
        }
        if(digits == 3){
            u32 r = (rgb >> 8) & 15, g = (rgb >> 4) & 15, b = rgb & 15;
            *out = ((r * 17) << 16) | ((g * 17) << 8) | (b * 17);
            return 1;
        }
        if(digits == 6){ *out = rgb; return 1; }
        return 0;
    }
    if(n >= 4 && c_lower(v[0]) == 'r' && c_lower(v[1]) == 'g' && c_lower(v[2]) == 'b'){
        u32 i = 3;                          /* rgb(...) or rgba(...) */
        if(i < n && c_lower(v[i]) == 'a') i++;
        while(i < n && v[i] != '(') i++;
        if(i >= n) return 0;
        i++;
        int comp[4], nc = 0;
        while(i < n && nc < 4){
            while(i < n && (v[i] == ' ' || v[i] == ',')) i++;
            if(i >= n || v[i] == ')') break;
            int val = 0, any = 0;
            while(i < n && v[i] >= '0' && v[i] <= '9'){ val = val * 10 + (v[i] - '0'); i++; any = 1; }
            if(i < n && v[i] == '.'){ i++; while(i < n && v[i] >= '0' && v[i] <= '9') i++; }  /* ignore alpha fraction */
            if(i < n && v[i] == '%'){ val = val * 255 / 100; i++; }
            if(any) comp[nc++] = val; else i++;
        }
        if(nc >= 3){
            for(int j = 0; j < 3; j++){ if(comp[j] < 0) comp[j] = 0; if(comp[j] > 255) comp[j] = 255; }
            *out = ((u32)comp[0] << 16) | ((u32)comp[1] << 8) | (u32)comp[2];
            return 1;
        }
        return 0;
    }
    for(u32 i = 0; i < sizeof colors / sizeof colors[0]; i++){
        u32 k = 0;
        while(k < n && colors[i].name[k] && c_lower(v[k]) == colors[i].name[k]) k++;
        if(k == n && colors[i].name[k] == 0){
            *out = colors[i].rgb;
            return 1;
        }
    }
    return 0;
}

/* A length in px: "12px", "1.5em", "2rem", "150%", "10pt" or "0". base is the font size. */
static int parse_len(const char *v, u32 n, int base, int *out){
    trim(&v, &n);
    u32 i = 0;
    int neg = 0;
    if(i < n && (v[i] == '-' || v[i] == '+')){ neg = v[i] == '-'; i++; }
    int whole = 0, tenth = 0, any = 0;
    while(i < n && v[i] >= '0' && v[i] <= '9'){ whole = whole * 10 + (v[i] - '0'); i++; any = 1; }
    if(i < n && v[i] == '.'){
        i++;
        if(i < n && v[i] >= '0' && v[i] <= '9'){ tenth = v[i] - '0'; any = 1; i++; }
        while(i < n && v[i] >= '0' && v[i] <= '9') i++;     /* extra digits are dropped */
    }
    if(!any) return 0;
    int t10 = whole * 10 + tenth;                           /* the value in tenths */
    char unit[4] = "";
    u32 k = 0;
    while(i < n && k + 1 < sizeof unit && ((v[i] >= 'a' && v[i] <= 'z') || v[i] == '%')) unit[k++] = v[i++];
    unit[k] = 0;
    int px;
    if(unit[0] == 0 || strcmp(unit, "px") == 0) px = t10 / 10;
    else if(strcmp(unit, "em") == 0 || strcmp(unit, "rem") == 0) px = t10 * base / 10;
    else if(strcmp(unit, "%") == 0) px = t10 * base / 1000;
    else if(strcmp(unit, "pt") == 0) px = t10 * 4 / 30;
    else return 0;
    *out = neg ? -px : px;
    return 1;
}

/* A width: some px plus tenths of a percent of the container (pct) and of the viewport (vw).
 * Takes "200px", "50%", "80vw", "auto" (all zero) and "calc(100% - 2rem)". */
static int parse_wlen(const char *v, u32 n, int base, int *px, int *pct, int *vw){
    trim(&v, &n);
    *px = *pct = *vw = 0;
    if(n >= 5 && v[0] == 'c' && v[1] == 'a' && v[2] == 'l' && v[3] == 'c' && v[4] == '('){
        /* calc(): terms joined by + and - (no * or /) */
        u32 i = 5;
        int sign = 1, any = 0;
        while(i < n && v[i] != ')'){
            while(i < n && v[i] == ' ') i++;
            if(i < n && (v[i] == '+' || v[i] == '-') && i + 1 < n && v[i + 1] == ' '){ sign = v[i] == '-' ? -1 : 1; i++; continue; }
            u32 b = i;
            while(i < n && v[i] != ' ' && v[i] != ')') i++;
            int a, c, d;
            if(i > b && parse_wlen(v + b, i - b, base, &a, &c, &d)){ *px += sign * a; *pct += sign * c; *vw += sign * d; any = 1; }
            sign = 1;
        }
        return any;
    }
    if(n >= 1 && v[n - 1] == '%'){
        int t = 0, whole = 0, tenth = 0, seen = 0;
        u32 i = 0;
        while(i < n && v[i] >= '0' && v[i] <= '9'){ whole = whole * 10 + (v[i] - '0'); i++; seen = 1; }
        if(i < n && v[i] == '.'){ i++; if(i < n && v[i] >= '0' && v[i] <= '9'){ tenth = v[i] - '0'; seen = 1; } }
        if(!seen) return 0;
        t = whole * 10 + tenth;
        *pct = t;
        return 1;
    }
    if(n >= 3 && v[n - 2] == 'v' && v[n - 1] == 'w'){
        int whole = 0; u32 i = 0;
        while(i < n && v[i] >= '0' && v[i] <= '9'){ whole = whole * 10 + (v[i] - '0'); i++; }
        if(i == 0) return 0;
        *vw = whole * 10;
        return 1;
    }
    return parse_len(v, n, base, px);
}

/* Parses 1-4 margin/padding values into the four sides (top, right, bottom, left). */
static void parse_box(const char *v, u32 n, int base, int side[4]){
    const char *p = v;
    u32 rem = n;
    int vals[4], cnt = 0;
    while(rem && cnt < 4){
        while(rem && c_ws(*p)){ p++; rem--; }
        const char *b = p;
        while(rem && !c_ws(*p)){ p++; rem--; }
        int px;
        if(p > b && parse_len(b, (u32)(p - b), base, &px)) vals[cnt++] = px;
        else if(p - b == 4 && b[0] == 'a' && b[1] == 'u' && b[2] == 't' && b[3] == 'o') vals[cnt++] = 0;
        else break;
    }
    if(cnt == 0) return;
    if(cnt == 1){ side[0] = side[1] = side[2] = side[3] = vals[0]; }
    else if(cnt == 2){ side[0] = side[2] = vals[0]; side[1] = side[3] = vals[1]; }
    else if(cnt == 3){ side[0] = vals[0]; side[1] = side[3] = vals[1]; side[2] = vals[2]; }
    else { side[0] = vals[0]; side[1] = vals[1]; side[2] = vals[2]; side[3] = vals[3]; }
}

/* ---- properties ---- */

static int named_size(const char *v, int *px){
    static const struct { const char *n; int px; } sz[] = {
        { "xx-small", 12 }, { "x-small", 16 }, { "small", 24 }, { "medium", 32 },
        { "large", 40 }, { "x-large", 48 }, { "xx-large", 64 }, { "smaller", 24 }, { "larger", 40 }
    };
    for(u32 i = 0; i < sizeof sz / sizeof sz[0]; i++)
        if(strcmp(v, sz[i].n) == 0){ *px = sz[i].px; return 1; }
    return 0;
}

/* Applies one "prop: value" pair to a style. parent_size is the font size of the parent. */
static void apply_one(wstyle_t *st, int parent_size, const char *prop, u32 pn, const char *val, u32 vn){
    char p[24];
    u32 k = 0;
    for(u32 i = 0; i < pn && k + 1 < sizeof p; i++) p[k++] = (char)c_lower(prop[i]);
    p[k] = 0;
    trim(&val, &vn);
    char v[64];
    k = 0;
    for(u32 i = 0; i < vn && k + 1 < sizeof v; i++) v[k++] = (char)c_lower(val[i]);
    v[k] = 0;
    /* drop "!important" */
    for(u32 i = 0; i + 9 <= k; i++) if(strcmp(v + i, "!important") == 0){ v[i] = 0; k = i; break; }
    u32 vl = k;

    if(strcmp(p, "color") == 0){
        u32 c; if(parse_color(v, vl, &c)) st->color = c;
    } else if(strcmp(p, "background-color") == 0 || strcmp(p, "background") == 0){
        u32 c;
        if(parse_color(v, vl, &c)){
            if(c == 0xFFFFFFFF){ st->bg_set = 0; }
            else if(c != 0xFFFFFFFE){ st->bg_set = 1; st->bg = c; }
        }
    } else if(strcmp(p, "font-size") == 0){
        int px;
        if(named_size(v, &px)) st->size = px;
        else if(parse_len(v, vl, parent_size, &px) && px > 0){ st->size = px; st->hide_text = 0; }
        else if(parse_len(v, vl, parent_size, &px) && px == 0) st->hide_text = 1;
    } else if(strcmp(p, "font-weight") == 0){
        if(strcmp(v, "bold") == 0 || strcmp(v, "bolder") == 0 || (vl == 3 && v[0] >= '6' && v[0] <= '9' && v[1] == '0' && v[2] == '0'))
            st->bold = 1;
        else if(strcmp(v, "normal") == 0 || strcmp(v, "lighter") == 0) st->bold = 0;
    } else if(strcmp(p, "font-style") == 0){
        st->italic = strcmp(v, "italic") == 0 || strcmp(v, "oblique") == 0;
    } else if(strcmp(p, "white-space") == 0){
        st->white_pre = strcmp(v, "pre") == 0 || strcmp(v, "pre-wrap") == 0 || strcmp(v, "pre-line") == 0;
    } else if(strcmp(p, "text-decoration") == 0 || strcmp(p, "text-decoration-line") == 0){
        st->underline = strcmp(v, "underline") == 0 ? 1 : (strcmp(v, "none") == 0 ? 0 : st->underline);
    } else if(strcmp(p, "text-align") == 0){
        if(strcmp(v, "center") == 0) st->align = 1;
        else if(strcmp(v, "right") == 0) st->align = 2;
        else if(strcmp(v, "left") == 0) st->align = 0;
    } else if(strcmp(p, "display") == 0){
        if(strcmp(v, "none") == 0) st->display = W_NONE;
        else if(strcmp(v, "block") == 0 || strcmp(v, "grid") == 0) st->display = W_BLOCK;
        else if(strcmp(v, "inline") == 0 || strcmp(v, "inline-block") == 0) st->display = W_INLINE;
        else if(strcmp(v, "list-item") == 0) st->display = W_LISTITEM;
        else if(strcmp(v, "flex") == 0 || strcmp(v, "inline-flex") == 0) st->display = W_FLEX;
    } else if(strcmp(p, "width") == 0){
        int px, pct, vw;
        if(parse_wlen(v, vl, st->size, &px, &pct, &vw)){ st->width = px; st->width_pct = pct; st->width_vw = vw; }
    } else if(strcmp(p, "max-width") == 0){
        int px, pct, vw;
        if(strcmp(v, "none") == 0){ st->max_w = st->max_pct = st->max_vw = 0; }
        else if(parse_wlen(v, vl, st->size, &px, &pct, &vw)){ st->max_w = px; st->max_pct = pct; st->max_vw = vw; }
    } else if(strcmp(p, "list-style") == 0 || strcmp(p, "list-style-type") == 0){
        st->no_bullet = strncmp(v, "none", 4) == 0;
    } else if(strcmp(p, "flex-grow") == 0){
        st->flex_grow = v[0] >= '1' && v[0] <= '9' ? v[0] - '0' : 0;
    } else if(strcmp(p, "flex") == 0){
        /* "1", "1 1 0", "auto" grow; "none", "0 ..." do not */
        if(strcmp(v, "auto") == 0) st->flex_grow = 1;
        else if(strcmp(v, "none") == 0) st->flex_grow = 0;
        else if(v[0] >= '1' && v[0] <= '9') st->flex_grow = v[0] - '0';
        else st->flex_grow = 0;
    } else if(strcmp(p, "flex-wrap") == 0){
        st->flex_wrap = strcmp(v, "wrap") == 0 || strcmp(v, "wrap-reverse") == 0;
    } else if(strcmp(p, "flex-direction") == 0){
        st->flex_col = strcmp(v, "column") == 0 || strcmp(v, "column-reverse") == 0;
    } else if(strcmp(p, "flex-flow") == 0){
        st->flex_col = v[0] == 'c';
        for(const char *q = v; *q; q++) if(*q == 'w' && q[1] == 'r') st->flex_wrap = 1;
    } else if(strcmp(p, "justify-content") == 0){
        st->justify = strcmp(v, "center") == 0 ? 1 :
                      (strcmp(v, "flex-end") == 0 || strcmp(v, "end") == 0 || strcmp(v, "right") == 0) ? 2 :
                      strcmp(v, "space-between") == 0 ? 3 :
                      (strcmp(v, "space-around") == 0 || strcmp(v, "space-evenly") == 0) ? 4 : 0;
    } else if(strcmp(p, "box-sizing") == 0){
        st->border_box = strcmp(v, "border-box") == 0;
    } else if(strcmp(p, "height") == 0 || strcmp(p, "min-height") == 0){
        int px;
        if(parse_len(v, vl, st->size, &px) && px > 0) st->height = px;
        else if(p[0] == 'h' && parse_len(v, vl, st->size, &px) && px == 0) st->zero_h = 1;
    } else if(strcmp(p, "overflow") == 0 || strcmp(p, "overflow-y") == 0){
        st->ovf_hidden = strncmp(v, "hidden", 6) == 0 || strncmp(v, "clip", 4) == 0;
    } else if(strcmp(p, "visibility") == 0){
        if(strcmp(v, "hidden") == 0 || strcmp(v, "collapse") == 0) st->display = W_NONE;
    } else if(strcmp(p, "text-indent") == 0){
        int px;
        if(parse_len(v, vl, st->size, &px)) st->hide_text = px < -500;
    } else if(strcmp(p, "border") == 0 || strcmp(p, "border-width") == 0 || strcmp(p, "border-color") == 0 ||
              strcmp(p, "border-top") == 0 || strcmp(p, "border-bottom") == 0 || strcmp(p, "border-left") == 0 ||
              strcmp(p, "border-right") == 0 || strcmp(p, "border-style") == 0 ||
              strcmp(p, "border-top-width") == 0 || strcmp(p, "border-right-width") == 0 ||
              strcmp(p, "border-bottom-width") == 0 || strcmp(p, "border-left-width") == 0){
        /* which sides this property sets: 0 = all four */
        int side = 0;   /* 1 top, 2 right, 3 bottom, 4 left */
        if(strcmp(p, "border-top") == 0 || strcmp(p, "border-top-width") == 0) side = 1;
        else if(strcmp(p, "border-right") == 0 || strcmp(p, "border-right-width") == 0) side = 2;
        else if(strcmp(p, "border-bottom") == 0 || strcmp(p, "border-bottom-width") == 0) side = 3;
        else if(strcmp(p, "border-left") == 0 || strcmp(p, "border-left-width") == 0) side = 4;
        int *dst[4] = { &st->bt, &st->br, &st->bb, &st->bl };
        /* the parts of a border value: a width, a colour, or a style word */
        u32 i = 0;
        while(i < vl){
            while(i < vl && v[i] == ' ') i++;
            u32 b = i;
            while(i < vl && v[i] != ' ') i++;
            if(i == b) break;
            int px; u32 c;
            char tk[24]; u32 tl = i - b < 23 ? i - b : 23;
            memcpy(tk, v + b, tl); tk[tl] = 0;
            if(strcmp(tk, "none") == 0 || strcmp(tk, "hidden") == 0){
                if(side) *dst[side - 1] = 0; else st->bt = st->br = st->bb = st->bl = 0;
            }
            else if(strcmp(tk, "solid") == 0 || strcmp(tk, "dashed") == 0 || strcmp(tk, "dotted") == 0){
                if(side){ if(*dst[side - 1] == 0) *dst[side - 1] = 1; }
                else for(int k = 0; k < 4; k++) if(dst[k][0] == 0) *dst[k] = 1;
            }
            else if(parse_len(v + b, i - b, st->size, &px)){
                if(side) *dst[side - 1] = px; else st->bt = st->br = st->bb = st->bl = px;
            }
            else if(parse_color(v + b, i - b, &c)) st->border_color = c;
        }
        st->border = st->bt > st->br ? st->bt : st->br;
        if(st->bb > st->border) st->border = st->bb;
        if(st->bl > st->border) st->border = st->bl;
    } else if(strcmp(p, "gap") == 0){
        int px;
        if(parse_len(v, vl, st->size, &px)) st->gap = px;
    } else if(strcmp(p, "margin") == 0){
        parse_box(v, vl, st->size, st->m);
        /* "auto" for the left and right margins: count the words */
        const char *w[4]; u32 wl_[4]; int nw = 0;
        for(u32 i = 0; i < vl && nw < 4; ){
            while(i < vl && v[i] == ' ') i++;
            if(i >= vl) break;
            w[nw] = v + i;
            u32 b = i;
            while(i < vl && v[i] != ' ') i++;
            wl_[nw++] = i - b;
        }
        #define IS_AUTO(k) (wl_[k] == 4 && w[k][0] == 'a' && w[k][1] == 'u')
        st->auto_lr = 0;
        if(nw == 1 && IS_AUTO(0)) st->auto_lr = 3;
        else if(nw == 2 || nw == 3){ if(IS_AUTO(1)) st->auto_lr = 3; }
        else if(nw == 4){ st->auto_lr = (IS_AUTO(1) ? 1 : 0) | (IS_AUTO(3) ? 2 : 0); }
        #undef IS_AUTO
    } else if(strcmp(p, "padding") == 0){
        parse_box(v, vl, st->size, st->p);
    } else if(strcmp(p, "margin-top") == 0 || strcmp(p, "margin-right") == 0 ||
              strcmp(p, "margin-bottom") == 0 || strcmp(p, "margin-left") == 0){
        int px;
        int side = p[7] == 't' ? 0 : p[7] == 'r' ? 1 : p[7] == 'b' ? 2 : 3;
        if(parse_len(v, vl, st->size, &px)) st->m[side] = px;
        else if(strcmp(v, "auto") == 0 && (side == 1 || side == 3)) st->auto_lr |= side == 1 ? 1 : 2;
    } else if(strcmp(p, "padding-top") == 0 || strcmp(p, "padding-right") == 0 ||
              strcmp(p, "padding-bottom") == 0 || strcmp(p, "padding-left") == 0){
        int px;
        int side = p[8] == 't' ? 0 : p[8] == 'r' ? 1 : p[8] == 'b' ? 2 : 3;
        if(parse_len(v, vl, st->size, &px)) st->p[side] = px;
    }
}

/* Applies a declaration block: "a: b; c: d". */
static void apply_block(wstyle_t *st, int parent_size, const char *d, u32 n){
    u32 i = 0;
    while(i < n){
        u32 ds = i;
        while(i < n && d[i] != ';') i++;
        u32 de = i;
        if(i < n) i++;
        u32 colon = ds;
        while(colon < de && d[colon] != ':') colon++;
        if(colon >= de) continue;
        const char *pn = d + ds; u32 pl = colon - ds;
        trim(&pn, &pl);
        apply_one(st, parent_size, pn, pl, d + colon + 1, de - colon - 1);
    }
}

/* ---- defaults, like a browser's stylesheet ---- */

static void block_margins(wstyle_t *st, int top, int bottom){
    st->m[0] = top; st->m[2] = bottom;
}

static void defaults(wnode_t *n){
    wstyle_t *st = &n->st;
    const char *t = n->tag;
    if(n->hidden){ st->display = W_NONE; return; }
    if(strcmp(t, "head") == 0 || strcmp(t, "script") == 0 || strcmp(t, "style") == 0 ||
       strcmp(t, "title") == 0 || strcmp(t, "meta") == 0 || strcmp(t, "link") == 0 ||
       strcmp(t, "noscript") == 0 || strcmp(t, "template") == 0){
        st->display = W_NONE;
        return;
    }
    if(strcmp(t, "body") == 0 || strcmp(t, "html") == 0 || strcmp(t, "div") == 0 || strcmp(t, "section") == 0 ||
       strcmp(t, "article") == 0 || strcmp(t, "main") == 0 || strcmp(t, "header") == 0 || strcmp(t, "footer") == 0 ||
       strcmp(t, "nav") == 0 || strcmp(t, "aside") == 0 || strcmp(t, "form") == 0 || strcmp(t, "table") == 0 ||
       strcmp(t, "center") == 0 || strcmp(t, "address") == 0 || strcmp(t, "figure") == 0 || strcmp(t, "#root") == 0 ||
       strcmp(t, "tr") == 0 || strcmp(t, "dl") == 0 || strcmp(t, "td") == 0 || strcmp(t, "th") == 0 ||
       strcmp(t, "thead") == 0 || strcmp(t, "tbody") == 0 || strcmp(t, "tfoot") == 0 || strcmp(t, "caption") == 0){
        st->display = W_BLOCK;
        if(strcmp(t, "center") == 0) st->align = 1;
        return;
    }
    if(strcmp(t, "p") == 0 || strcmp(t, "dd") == 0 || strcmp(t, "dt") == 0){
        st->display = W_BLOCK;
        block_margins(st, 16, 16);
        return;
    }
    if(t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0){
        static const int hs[6] = { 64, 48, 40, 32, 32, 32 };
        static const int hm[6] = { 22, 20, 18, 16, 16, 16 };
        int lvl = t[1] - '1';
        st->display = W_BLOCK;
        st->size = hs[lvl];
        st->bold = 1;
        st->italic = lvl == 5;
        block_margins(st, hm[lvl], hm[lvl]);
        return;
    }
    if(strcmp(t, "ul") == 0 || strcmp(t, "ol") == 0){
        st->display = W_BLOCK;
        block_margins(st, 16, 16);
        st->p[3] = 40;
        return;
    }
    if(strcmp(t, "li") == 0){
        st->display = W_LISTITEM;
        return;
    }
    if(strcmp(t, "blockquote") == 0){
        st->display = W_BLOCK;
        block_margins(st, 16, 16);
        st->p[3] = 24;
        st->bl = 4;                /* a left accent bar, as browsers draw */
        st->border_color = 0xC2CCD4;
        st->color = 0x5C6670;      /* a muted tone for the quoted text */
        return;
    }
    if(strcmp(t, "pre") == 0){
        st->display = W_BLOCK;
        block_margins(st, 16, 16);
        st->size = 24;
        st->white_pre = 1;         /* keep newlines and spaces */
        return;
    }
    if(strcmp(t, "hr") == 0){
        st->display = W_BLOCK;
        block_margins(st, 12, 12);
        return;
    }
    if(strcmp(t, "b") == 0 || strcmp(t, "strong") == 0 || strcmp(t, "th") == 0){
        st->bold = 1;
    } else if(strcmp(t, "i") == 0 || strcmp(t, "em") == 0 || strcmp(t, "cite") == 0){
        st->italic = 1;
    } else if(strcmp(t, "u") == 0 || strcmp(t, "ins") == 0){
        st->underline = 1;
    } else if(strcmp(t, "a") == 0){
        st->color = 0x6CB4FF;
        st->underline = 1;
    } else if(strcmp(t, "small") == 0){
        st->size = 24;
    } else if(strcmp(t, "big") == 0){
        st->size = 40;
    }
    if(st->display == W_NONE) st->display = W_INLINE;
}

/* Matching rules sorted by specificity, then by document order. */
void css_apply(wnode_t *n, wnode_t *parent){
    wstyle_t *st = &n->st;
    int psize = 32;
    if(parent){
        *st = parent->st;
        psize = parent->st.size;
        /* properties that are not inherited */
        st->bg_set = 0;
        memset(st->m, 0, sizeof st->m);
        memset(st->p, 0, sizeof st->p);
        st->underline = 0;
        st->display = W_INLINE;
        st->width = 0; st->height = 0; st->border = 0; st->bt = st->br = st->bb = st->bl = 0; st->gap = 0;
        st->width_pct = st->width_vw = 0;
        st->max_w = st->max_pct = st->max_vw = 0;
        st->auto_lr = 0; st->border_box = 0;
        st->flex_grow = st->flex_wrap = st->flex_col = st->justify = 0;
        st->zero_h = st->ovf_hidden = 0;
    } else {
        memset(st, 0, sizeof *st);
        st->color = 0xF2F5F7;
        st->size = 32;
        st->display = W_BLOCK;
    }
    if(n->text) return;
    defaults(n);
    if(st->display == W_NONE) return;

    static int order[RULES_MAX];       /* static: too big for the kernel stack (not re-entered) */
    int nm = 0;
    n->sig = node_sig(n);
    use_sig = 1;
    for(int i = 0; i < nrules; i++){
        const rule_t *r = &rules[i];
        const compound_t *last = &r->c[r->n - 1];
        if(last->never || (n->sig & last->need) != last->need) continue;      /* most rules stop here */
        if((r->min_w && view_width < r->min_w) || (r->max_w && view_width > r->max_w)) continue;
        if(selector_match(n, r)) order[nm++] = i;
    }
    use_sig = 0;
    /* insertion sort by (specificity, order) */
    for(int i = 1; i < nm; i++){
        int v = order[i], j = i - 1;
        while(j >= 0){
            rule_t *a = &rules[order[j]], *b = &rules[v];
            if(a->spec < b->spec || (a->spec == b->spec && a->order <= b->order)) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    for(int i = 0; i < nm; i++)
        if(rules[order[i]].decl) apply_block(st, psize, rules[order[i]].decl, (u32)strlen(rules[order[i]].decl));
    if(n->style) apply_block(st, psize, n->style, (u32)strlen(n->style));
    if(st->zero_h && st->ovf_hidden) st->display = W_NONE;     /* a collapsed box: its content is clipped away */
    if(n->hidden) st->display = W_NONE;
    /* the font size an em refers to is the one just computed for this element */
    if(st->size < 1) st->size = 1;
}

int css_tag_is_hidden(const char *tag){
    return strcmp(tag, "head") == 0 || strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}
