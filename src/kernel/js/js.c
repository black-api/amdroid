/* JavaScript: tokenizer, parser and tree-walking evaluator. Memory comes from one arena
 * that js_init() empties (one page at a time, no garbage collector). See js.h. */
#include "amdroid/js.h"
#include "amdroid/heap.h"
#include "amdroid/kmath.h"
#include "amdroid/timer.h"
#include "string.h"
#include "stdint.h"

#define ARENA_SIZE   (8 * 1024 * 1024)
#define MAX_DEPTH    200
#define MAX_STEPS    20000000
#define SCRATCH_SIZE 65536

int re_search(const char *pat, u32 plen, int icase, const char *text, u32 tlen, u32 start, u32 *ms, u32 *me);
void js_fail_hook_internal(const char *msg);

/* ---- memory: a mark-and-sweep heap over one arena ----
 * Every allocation has a header (size, flags, free-list link). A collection marks what
 * the program can still reach: it scans the C stack and the registered global roots
 * conservatively (any word that looks like a pointer into a live block keeps it), then
 * frees every unmarked block and merges neighbouring free blocks. */
typedef struct hdr { u32 size; u32 flags; struct hdr *nfree; } hdr_t;
#define HDR_SZ   ((u32)sizeof(hdr_t))
#define F_USED   1u
#define F_MARK   2u
#define MSTACK   8192
#define MAX_ROOTS 128

static char *arena = NULL, *arena_end = NULL;
static hdr_t *free_list = NULL;
static char scratch[SCRATCH_SIZE];
static int in_gc = 0;
static struct { void *p; u32 n; } roots[MAX_ROOTS];
static int nroots = 0;
static hdr_t *mstack[MSTACK];
static u32 msp = 0;
static int mover = 0;
static hdr_t **starts = NULL;
static u32 nstarts = 0;
extern char stack_top[];          /* the top of the kernel stack (boot.asm) */

/* Registers a variable or array that holds pointers into the heap. */
void js_gc_root(void *p, u32 n){
    if(nroots < MAX_ROOTS){ roots[nroots].p = p; roots[nroots].n = n; nroots++; }
}

static void heap_reset(void){
    if(!arena) arena = kmalloc(ARENA_SIZE);
    arena_end = arena + ARENA_SIZE;
    hdr_t *h = (hdr_t *)arena;
    h->size = ARENA_SIZE - HDR_SZ;
    h->flags = 0;
    h->nfree = NULL;
    free_list = h;
    nroots = 0;
}

static void *try_alloc(u32 n){
    hdr_t **pp = &free_list;
    for(hdr_t *h = free_list; h; pp = &h->nfree, h = h->nfree){
        if(h->size < n) continue;
        if(h->size >= n + HDR_SZ + 16){
            hdr_t *rest = (hdr_t *)((char *)h + HDR_SZ + n);
            rest->size = h->size - n - HDR_SZ;
            rest->flags = 0;
            rest->nfree = h->nfree;
            h->size = n;
            *pp = rest;
        } else {
            *pp = h->nfree;
        }
        h->flags = F_USED;
        h->nfree = NULL;
        return (char *)h + HDR_SZ;
    }
    return NULL;
}

static void gc(void);

static void *jalloc(u32 n){
    n = (n + 7) & ~7u;
    if(n < 16) n = 16;
    if(!arena) heap_reset();
    void *p = try_alloc(n);
    if(!p && !in_gc){
        gc();
        p = try_alloc(n);
    }
    if(!p){
        js_fail_hook_internal("out of memory");
        memset(scratch, 0, sizeof scratch);
        return scratch;
    }
    return p;
}

static const char *jstrdup(const char *s, u32 n){
    char *d = jalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* If w points into a live block, marks that block and queues it. */
static void mark_addr(uintptr_t w){
    if(w < (uintptr_t)arena || w >= (uintptr_t)arena_end) return;
    int lo = 0, hi = (int)nstarts - 1, best = -1;
    while(lo <= hi){
        int mid = (lo + hi) / 2;
        if((uintptr_t)starts[mid] <= w){ best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if(best < 0) return;
    hdr_t *h = starts[best];
    if(!(h->flags & F_USED) || (h->flags & F_MARK)) return;
    if(w < (uintptr_t)h + HDR_SZ || w >= (uintptr_t)h + HDR_SZ + h->size) return;
    h->flags |= F_MARK;
    if(msp < MSTACK) mstack[msp++] = h;
    else mover = 1;
}

static void scan_words(const void *p, u32 bytes){
    const uintptr_t *w = (const uintptr_t *)p;
    for(u32 i = 0; i < bytes / sizeof(uintptr_t); i++) mark_addr(w[i]);
}

static void drain(void){
    while(msp > 0){
        hdr_t *h = mstack[--msp];
        scan_words((char *)h + HDR_SZ, h->size);
    }
}

static void gc(void){
    in_gc = 1;
    /* every block, in address order */
    u32 nb = 0;
    for(char *p = arena; p < arena_end; ){
        hdr_t *h = (hdr_t *)p;
        nb++;
        p += HDR_SZ + h->size;
    }
    starts = kmalloc(sizeof(hdr_t *) * nb);
    nstarts = 0;
    if(starts){
        for(char *p = arena; p < arena_end; ){
            hdr_t *h = (hdr_t *)p;
            starts[nstarts++] = h;
            p += HDR_SZ + h->size;
        }
    }
    msp = 0;
    mover = 0;
    /* roots: the stack (spilling registers first), then the registered globals */
    __builtin_unwind_init();
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    for(uintptr_t a = here & ~(uintptr_t)7; a < (uintptr_t)stack_top; a += 8)
        mark_addr(*(uintptr_t *)a);
    for(int i = 0; i < nroots; i++) scan_words(roots[i].p, roots[i].n);
    drain();
    /* if the mark stack overflowed, re-scan the marked blocks until nothing new turns up */
    while(mover){
        mover = 0;
        for(u32 i = 0; i < nstarts; i++){
            hdr_t *h = starts[i];
            if(h->flags & F_MARK){ scan_words((char *)h + HDR_SZ, h->size); drain(); }
        }
    }
    /* sweep: free the unmarked blocks, merging neighbours, and rebuild the free list */
    free_list = NULL;
    hdr_t **tail = &free_list;
    hdr_t *prev_free = NULL;
    for(char *p = arena; p < arena_end; ){
        hdr_t *h = (hdr_t *)p;
        u32 next = HDR_SZ + h->size;
        if((h->flags & F_USED) && (h->flags & F_MARK)){
            h->flags &= ~F_MARK;
            prev_free = NULL;
        } else if(prev_free){
            prev_free->size += next;      /* absorbed into the free block before */
        } else {
            h->flags = 0;
            h->nfree = NULL;
            *tail = h;
            tail = &h->nfree;
            prev_free = h;
        }
        p += next;
    }
    if(starts){ kfree(starts); starts = NULL; }
    nstarts = 0;
    in_gc = 0;
}

/* ---- errors ---- */
static char err_msg[160];
static int failed = 0;        /* a runtime error (catchable by try) */
static js_val_t thrown_val;   /* a throw statement */
static int thrown = 0;
static char last_log[160];
static int prevented = 0;

void js_fail_hook_internal(const char *msg){
    if(!failed){
        failed = 1;
        u32 i = 0;
        while(msg[i] && i + 1 < sizeof err_msg){ err_msg[i] = msg[i]; i++; }
        err_msg[i] = 0;
    }
}
static void fail(const char *msg){ js_fail_hook_internal(msg); }

static void ui_copy_js(char *dst, const char *src, u32 n){
    u32 i = 0;
    while(i + 1 < n && src[i]){ dst[i] = src[i]; i++; }
    dst[i] = 0;
}
#define STOP() (failed || thrown)

const char *js_error(void){ return err_msg; }
const char *js_last_log(void){ return last_log; }
int js_take_prevented(void){ int p = prevented; prevented = 0; return p; }
void js_mark_prevented(void){ prevented = 1; }

/* ---- values ---- */
js_val_t js_undef(void){ js_val_t v; memset(&v, 0, sizeof v); v.t = JS_UNDEF; return v; }
js_val_t js_null(void){ js_val_t v = js_undef(); v.t = JS_NULL; return v; }
js_val_t js_bool(int b){ js_val_t v = js_undef(); v.t = JS_BOOL; v.b = b ? 1 : 0; return v; }
js_val_t js_num(double n){ js_val_t v = js_undef(); v.t = JS_NUM; v.n = n; return v; }
js_val_t js_str(const char *s){ js_val_t v = js_undef(); v.t = JS_STR; v.s = s; return v; }
js_val_t js_objv(js_obj_t *o){ js_val_t v = js_undef(); v.t = JS_OBJ; v.o = o; return v; }

static double nan_value(void){ volatile double z = 0; return z / z; }

/* ---- objects ---- */
typedef struct prop { const char *k; js_val_t v; struct prop *next; js_obj_t *getter, *setter; } prop_t;
typedef struct binding { const char *name; js_val_t v; struct binding *next; } binding_t;
typedef struct env { struct env *parent; binding_t *first; } env_t;
typedef struct jsnode jsnode_t;

typedef struct regex_data { const char *pat; u32 plen; int icase, global; } regex_data_t;

struct js_obj {
    int is_array, is_func, is_regex, is_class, default_ctor;
    prop_t *props;
    js_obj_t *proto;          /* the prototype (for lookups) */
    js_val_t *items;          /* arrays */
    int nitems, cap;
    jsnode_t *fdecl;          /* script functions */
    env_t *closure;
    js_obj_t *parent;         /* a class's superclass (constructor) */
    js_obj_t *home;           /* a method's class prototype (for super) */
    js_native_fn native;
    void *ctx;
    js_get_fn hget;
    js_set_fn hset;
    void *host;
    regex_data_t *re;
};

static js_obj_t *global_obj = NULL;
static env_t *global_env = NULL;
static js_obj_t *array_proto = NULL, *string_proto = NULL, *regex_proto = NULL, *object_proto = NULL;
static js_obj_t *number_proto = NULL;

js_obj_t *js_global(void){ return global_obj; }

static js_obj_t *new_obj(void){
    js_obj_t *o = jalloc(sizeof(js_obj_t));
    memset(o, 0, sizeof *o);
    return o;
}
js_obj_t *js_new_object(void){ js_obj_t *o = new_obj(); o->proto = object_proto; return o; }
js_obj_t *js_new_native(js_native_fn fn, void *ctx){
    js_obj_t *o = new_obj();
    o->is_func = 1;
    o->native = fn;
    o->ctx = ctx;
    return o;
}
void js_set_hooks(js_obj_t *o, js_get_fn get, js_set_fn set, void *host){
    o->hget = get; o->hset = set; o->host = host;
}
void *js_host(js_obj_t *o){ return o ? o->host : NULL; }

static prop_t *find_own(js_obj_t *o, const char *k){
    for(prop_t *p = o->props; p; p = p->next) if(strcmp(p->k, k) == 0) return p;
    return NULL;
}

void js_set(js_obj_t *o, const char *name, js_val_t v){
    prop_t *p = find_own(o, name);
    if(!p){
        p = jalloc(sizeof(prop_t));
        p->k = jstrdup(name, (u32)strlen(name));
        p->next = o->props;
        o->props = p;
    }
    p->v = v;
}

static void prop_remove(js_obj_t *o, const char *k){
    prop_t **pp = &o->props;
    while(*pp){
        if(strcmp((*pp)->k, k) == 0){ *pp = (*pp)->next; return; }
        pp = &(*pp)->next;
    }
}

static void arr_push(js_obj_t *a, js_val_t v){
    if(a->nitems == a->cap){
        int nc = a->cap ? a->cap * 2 : 8;
        js_val_t *ni = jalloc(sizeof(js_val_t) * (u32)nc);
        if(a->nitems) memcpy(ni, a->items, sizeof(js_val_t) * (u32)a->nitems);
        a->items = ni;
        a->cap = nc;
    }
    a->items[a->nitems++] = v;
}

static js_obj_t *new_array(void){
    js_obj_t *a = new_obj();
    a->is_array = 1;
    a->proto = array_proto;
    return a;
}
js_obj_t *js_new_array(void){ return new_array(); }
void js_array_push(js_obj_t *a, js_val_t v){ arr_push(a, v); }

static int as_index(const char *s){
    if(!s[0]) return -1;
    int n = 0;
    for(const char *p = s; *p; p++){
        if(*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
        if(n > 1000000) return -1;
    }
    return n;
}

static int is_str(const char *a, const char *b){ return strcmp(a, b) == 0; }
static js_val_t call_value(js_val_t fn, js_val_t self, int argc, js_val_t *argv);

/* Looks a name up on the object and its prototypes. */
js_val_t js_get(js_obj_t *o, const char *name){
    js_val_t out = js_undef();
    for(js_obj_t *x = o; x; x = x->proto){
        if(x->hget && x->hget(x, name, &out)) return out;
        if(x->is_array){
            if(is_str(name, "length")) return js_num(x->nitems);
            int i = as_index(name);
            if(i >= 0) return i < x->nitems ? x->items[i] : js_undef();
        }
        prop_t *p = find_own(x, name);
        if(p){
            if(p->getter) return call_value(js_objv(p->getter), js_objv(o), 0, NULL);
            if(p->setter && !p->getter) return js_undef();
            return p->v;
        }
    }
    return js_undef();
}

static int has_prop(js_obj_t *o, const char *name){
    for(js_obj_t *x = o; x; x = x->proto){
        if(find_own(x, name)) return 1;
        if(x->is_array){
            if(is_str(name, "length")) return 1;
            int i = as_index(name);
            if(i >= 0 && i < x->nitems) return 1;
        }
    }
    return 0;
}

static void obj_set(js_obj_t *o, const char *name, js_val_t v){
    if(o->hset && o->hset(o, name, v)) return;
    prop_t *ap = find_own(o, name);
    if(ap && (ap->setter || ap->getter)){
        if(ap->setter){ js_val_t a[1] = { v }; call_value(js_objv(ap->setter), js_objv(o), 1, a); }
        return;
    }
    if(o->is_array){
        int i = as_index(name);
        if(i >= 0){
            while(o->nitems <= i) arr_push(o, js_undef());
            o->items[i] = v;
            return;
        }
    }
    js_set(o, name, v);
}

/* ---- conversions ---- */
int js_truthy(js_val_t v){
    switch(v.t){
        case JS_BOOL: return v.b;
        case JS_NUM: return v.n != 0 && v.n == v.n;
        case JS_STR: return v.s[0] != 0;
        case JS_OBJ: return 1;
        default: return 0;
    }
}

double js_to_num(js_val_t v){
    switch(v.t){
        case JS_NUM: return v.n;
        case JS_BOOL: return v.b ? 1 : 0;
        case JS_STR: {
            if(v.s[0] == 0) return 0;
            double d = 0; const char *end = NULL;
            if(kstrtod(v.s, &d, &end) && end && *end == 0) return d;
            return nan_value();
        }
        case JS_NULL: return 0;
        case JS_UNDEF: return nan_value();
        default: return nan_value();
    }
}

const char *js_to_str(js_val_t v){
    char buf[40];
    switch(v.t){
        case JS_STR: return v.s;
        case JS_NUM:
            if(v.n != v.n) return "NaN";
            kfmt_double(buf, sizeof buf, v.n);
            return jstrdup(buf, (u32)strlen(buf));
        case JS_BOOL: return v.b ? "true" : "false";
        case JS_NULL: return "null";
        case JS_OBJ:
            if(!v.o) return "[object Object]";
            if(v.o->is_array){
                /* arrays print their items joined by commas */
                u32 n = 0;
                char *out = jalloc(4096);
                for(int i = 0; i < v.o->nitems && n + 2 < 4096; i++){
                    if(i) out[n++] = ',';
                    const char *s = js_to_str(v.o->items[i]);
                    for(; *s && n + 2 < 4096; s++) out[n++] = *s;
                }
                out[n] = 0;
                return out;
            }
            if(v.o->is_func) return "function";
            return "[object Object]";
        default: return "undefined";
    }
}

int js_callable(js_val_t v){ return v.t == JS_OBJ && v.o != NULL && v.o->is_func; }

/* ---- lexer ---- */
enum { T_EOF, T_NUMBER, T_STRING, T_ID, T_PUNCT, T_REGEX, T_TEMPLATE };
typedef struct { int t; double n; const char *s; const char *flags; } tok_t;

static tok_t *toks = NULL;
static int ntok = 0, ti = 0;

static int is_idc(int c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (c >= '0' && c <= '9'); }

/* After these, a '/' is division; anywhere else it starts a regex literal. */
static int value_before(const tok_t *t){
    if(t->t == T_NUMBER || t->t == T_STRING || t->t == T_REGEX || t->t == T_TEMPLATE) return 1;
    if(t->t == T_ID){
        static const char *kw[] = { "return", "typeof", "case", "in", "of", "instanceof", "new", "throw", "else", 0 };
        for(int i = 0; kw[i]; i++) if(strcmp(t->s, kw[i]) == 0) return 0;
        return 1;
    }
    if(t->t == T_PUNCT) return strcmp(t->s, ")") == 0 || strcmp(t->s, "]") == 0 || strcmp(t->s, "}") == 0;
    return 0;
}

static u32 tok_cap = 0;

/* Room for two more tokens (the array grows by doubling). 0 when memory ran out. */
static int tok_room(void){
    if((u32)ntok + 2 < tok_cap) return 1;
    u32 nc = tok_cap * 2;
    tok_t *nt = jalloc(sizeof(tok_t) * nc);
    if((char *)nt == scratch) return 0;          /* out of memory */
    memcpy(nt, toks, sizeof(tok_t) * (u32)ntok);
    toks = nt;                        /* the old array is garbage now */
    tok_cap = nc;
    return 1;
}

static void lex(const char *src, u32 len){
    tok_cap = len / 4 + 64;
    toks = jalloc(sizeof(tok_t) * tok_cap);
    ntok = 0;
    u32 i = 0;
    while(i < len && !STOP()){
        char c = src[i];
        if(c == ' ' || c == '\t' || c == '\n' || c == '\r'){ i++; continue; }
        if(c == '/' && i + 1 < len && src[i+1] == '/'){ while(i < len && src[i] != '\n') i++; continue; }
        if(c == '/' && i + 1 < len && src[i+1] == '*'){
            i += 2;
            while(i + 1 < len && !(src[i] == '*' && src[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        if(!tok_room()){ fail("script too large"); return; }
        tok_t *t = &toks[ntok];
        memset(t, 0, sizeof *t);
        if(c >= '0' && c <= '9'){
            double v = 0; const char *end = NULL;
            char tmp[48]; u32 k = 0;
            while(i + k < len && k < sizeof tmp - 1 && (is_idc(src[i+k]) || src[i+k] == '.')) { tmp[k] = src[i+k]; k++; }
            tmp[k] = 0;
            if(!kstrtod(tmp, &v, &end)) v = 0;
            t->t = T_NUMBER; t->n = v;
            i += end ? (u32)(end - tmp) : k;
            ntok++;
            continue;
        }
        if(is_idc(c)){
            u32 s = i;
            while(i < len && is_idc(src[i])) i++;
            t->t = T_ID;
            t->s = jstrdup(src + s, i - s);
            ntok++;
            continue;
        }
        if(c == '"' || c == '\''){
            char q = c;
            i++;
            u32 e0 = i;                       /* find the end first: the copy is never longer */
            while(e0 < len && src[e0] != q){ if(src[e0] == '\\' && e0 + 1 < len) e0++; e0++; }
            char *buf = jalloc(e0 - i + 1);
            t = &toks[ntok];                  /* (a collection cannot move it, but stay safe) */
            u32 n = 0;
            while(i < len && src[i] != q){
                if(src[i] == '\\' && i + 1 < len){
                    i++;
                    char e = src[i];
                    buf[n++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                    i++;
                    continue;
                }
                buf[n++] = src[i++];
            }
            buf[n] = 0;
            i++;
            t->t = T_STRING;
            t->s = buf;
            ntok++;
            continue;
        }
        if(c == '`'){
            /* a template literal: its raw text (with ${...} parts) is parsed later */
            u32 s0 = ++i;
            int tdepth = 0;
            while(i < len && !(src[i] == '`' && tdepth == 0)){
                if(src[i] == '\\' && i + 1 < len){ i += 2; continue; }
                if(src[i] == '{') tdepth++;
                if(src[i] == '}') tdepth--;
                i++;
            }
            t->t = T_TEMPLATE;
            t->s = jstrdup(src + s0, i - s0);
            i++;
            ntok++;
            continue;
        }
        if(c == '/' && !(ntok > 0 && value_before(&toks[ntok - 1]))){
            /* a regex literal: /pattern/flags */
            u32 s = ++i;
            while(i < len && src[i] != '/'){
                if(src[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            u32 pl = i - s;
            char *pat = jalloc(pl + 1);
            memcpy(pat, src + s, pl);
            pat[pl] = 0;
            i++;
            u32 fs = i;
            while(i < len && is_idc(src[i])) i++;
            char *fl = jalloc(i - fs + 1);
            memcpy(fl, src + fs, i - fs);
            fl[i - fs] = 0;
            t->t = T_REGEX;
            t->s = pat;
            t->flags = fl;
            ntok++;
            continue;
        }
        static const char *ops[] = { ">>>=", "===", "!==", "...", "**=", "?\?=", "==", "!=", "<=", ">=", "&&", "||",
                                     "?\?", "++", "--", "+=", "-=", "*=", "/=", "=>", "**", 0 };
        int matched = 0;
        /* '?.' optional chaining, but keep '?' before a decimal (a ? .5 : b) as a ternary */
        if(src[i] == '?' && i + 1 < len && src[i+1] == '.' &&
           !(i + 2 < len && src[i+2] >= '0' && src[i+2] <= '9')){
            t->t = T_PUNCT; t->s = "?."; i += 2; matched = 1;
        }
        for(int k = 0; !matched && ops[k]; k++){
            u32 n = (u32)strlen(ops[k]);
            if(i + n <= len && memcmp(src + i, ops[k], n) == 0){
                t->t = T_PUNCT;
                t->s = ops[k];                /* a constant: no copy needed */
                i += n;
                matched = 1;
                break;
            }
        }
        if(!matched){
            static char single[128][2];
            unsigned char ch = (unsigned char)src[i];
            t->t = T_PUNCT;
            if(ch < 128){ single[ch][0] = (char)ch; single[ch][1] = 0; t->s = single[ch]; }
            else t->s = jstrdup(src + i, 1);
            i++;
        }
        ntok++;
    }
    tok_t *e = &toks[ntok++];
    memset(e, 0, sizeof *e);
    e->t = T_EOF;
}

/* ---- AST ---- */
enum {
    N_NUM = 1, N_STR, N_IDENT, N_TRUE, N_FALSE, N_NULL, N_UNDEF, N_THIS, N_SUPER, N_ARRAY, N_OBJECT,
    N_FUNC, N_MEMBER, N_CALL, N_NEW, N_UNARY, N_BINARY, N_LOGIC, N_ASSIGN, N_COND, N_REGEX, N_CLASS,
    N_DECL, N_VAR, N_EXPR, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN, N_RETURN, N_BLOCK, N_FUNCDECL,
    N_BREAK, N_CONTINUE, N_EMPTY, N_THROW, N_TRY, N_SWITCH, N_CASE, N_SPREAD
};
enum {
    OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EQ, OP_NE, OP_SEQ, OP_SNE,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_AND, OP_OR, OP_NOT, OP_NEG, OP_TYPEOF, OP_POS,
    OP_SET, OP_ADDSET, OP_SUBSET, OP_INSTANCEOF, OP_IN, OP_DELETE, OP_VOID, OP_POW, OP_NULLISH
};

struct jsnode {
    int k, op;
    double num;
    const char *str, *flags;
    jsnode_t *a, *b, *c, *d;
    jsnode_t *list;
    jsnode_t *next;
    int computed, arrow;
};

static jsnode_t *node(int k){
    jsnode_t *n = jalloc(sizeof(jsnode_t));
    memset(n, 0, sizeof *n);
    n->k = k;
    return n;
}

static int tis(const char *p){ return toks[ti].t == T_PUNCT && strcmp(toks[ti].s, p) == 0; }
static int tkw(const char *w){ return toks[ti].t == T_ID && strcmp(toks[ti].s, w) == 0; }
static void adv(void){ if(ti < ntok - 1) ti++; }
static void expect(const char *p){
    if(tis(p)){ adv(); return; }
    char m[64] = "expected ";
    u32 k = (u32)strlen(m);
    for(const char *q = p; *q && k < 60; q++) m[k++] = *q;
    m[k] = 0;
    fail(m);
}

static jsnode_t *expression(void);
static jsnode_t *assignment(void);
static jsnode_t *statement(void);
static jsnode_t *block_body(void);

static int prec(const char *op){
    if(strcmp(op, "??") == 0) return 1;
    if(strcmp(op, "||") == 0) return 1;
    if(strcmp(op, "&&") == 0) return 2;
    if(strcmp(op, "**") == 0) return 7;
    if(strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "===") == 0 || strcmp(op, "!==") == 0) return 3;
    if(strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) return 4;
    if(strcmp(op, "+") == 0 || strcmp(op, "-") == 0) return 5;
    if(strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) return 6;
    return 0;
}

static int binop(const char *s){
    static const struct { const char *s; int op; } t[] = {
        { "+", OP_ADD }, { "-", OP_SUB }, { "*", OP_MUL }, { "/", OP_DIV }, { "%", OP_MOD },
        { "==", OP_EQ }, { "!=", OP_NE }, { "===", OP_SEQ }, { "!==", OP_SNE }, { "<", OP_LT },
        { ">", OP_GT }, { "<=", OP_LE }, { ">=", OP_GE }, { "&&", OP_AND }, { "||", OP_OR },
        { "instanceof", OP_INSTANCEOF }, { "in", OP_IN },
        { "**", OP_POW }, { "??", OP_NULLISH },
    };
    for(u32 i = 0; i < sizeof t / sizeof t[0]; i++) if(strcmp(s, t[i].s) == 0) return t[i].op;
    return OP_NONE;
}

static jsnode_t *function_rest(const char *name){
    jsnode_t *f = node(N_FUNC);
    f->str = name;
    expect("(");
    jsnode_t *last = NULL;
    while(!tis(")") && toks[ti].t != T_EOF && !STOP()){
        int rest = 0;
        if(tis("...")){ adv(); rest = 1; }
        if(toks[ti].t != T_ID){ fail("bad parameter"); return f; }
        jsnode_t *p = node(N_IDENT);
        p->str = toks[ti].s;
        p->op = rest ? 1 : 0;            /* op==1 marks a rest parameter */
        adv();
        if(!rest && tis("=")){ adv(); p->b = assignment(); }   /* default value */
        if(last) last->next = p; else f->list = p;
        last = p;
        if(tis(",")) adv();
    }
    expect(")");
    f->a = block_body();
    return f;
}

static jsnode_t *arrow_body(jsnode_t *f){
    f->arrow = 1;
    if(tis("{")){
        f->a = block_body();
    } else {
        jsnode_t *ret = node(N_RETURN);
        ret->a = assignment();
        jsnode_t *b = node(N_BLOCK);
        b->list = ret;
        f->a = b;
    }
    return f;
}

/* Parses an expression from a separate piece of source (used for ${...} in templates). */
static jsnode_t *parse_sub(const char *src, u32 len){
    tok_t *save_toks = toks;
    int save_n = ntok, save_ti = ti;
    lex(src, len);
    ti = 0;
    jsnode_t *e = expression();
    toks = save_toks; ntok = save_n; ti = save_ti;
    return e;
}

static jsnode_t *template_node(const char *raw){
    u32 n = (u32)strlen(raw);
    jsnode_t *res = NULL;
    char *chunk = jalloc(n + 1);
    u32 cn = 0;
    for(u32 i = 0; i <= n; ){
        if(i < n && raw[i] == '$' && i + 1 < n && raw[i+1] == '{'){
            /* flush the text so far */
            chunk[cn] = 0;
            jsnode_t *lit = node(N_STR);
            lit->str = jstrdup(chunk, cn);
            res = res ? ({ jsnode_t *b = node(N_BINARY); b->op = OP_ADD; b->a = res; b->b = lit; b; }) : lit;
            cn = 0;
            /* the expression up to its matching brace */
            u32 j = i + 2, depth = 1, es = j;
            while(j < n && depth > 0){
                if(raw[j] == '{') depth++;
                else if(raw[j] == '}') depth--;
                if(depth > 0) j++;
            }
            jsnode_t *e = parse_sub(raw + es, j - es);
            jsnode_t *b = node(N_BINARY);
            b->op = OP_ADD;
            b->a = res ? res : node(N_STR);
            if(!res){ b->a->str = ""; }
            b->b = e;
            res = b;
            i = j + 1;
            continue;
        }
        if(i < n){
            char c = raw[i];
            if(c == '\\' && i + 1 < n){
                char e = raw[i+1];
                chunk[cn++] = e == 'n' ? '\n' : e == 't' ? '\t' : e;
                i += 2;
                continue;
            }
            chunk[cn++] = c;
        }
        i++;
    }
    chunk[cn] = 0;
    jsnode_t *tail = node(N_STR);
    tail->str = jstrdup(chunk, cn);
    if(!res) return tail;
    jsnode_t *b = node(N_BINARY);
    b->op = OP_ADD;
    b->a = res;
    b->b = tail;
    return b;
}

static jsnode_t *primary(void){
    tok_t *t = &toks[ti];
    if(t->t == T_TEMPLATE){ jsnode_t *n = template_node(t->s); adv(); return n; }
    if(t->t == T_NUMBER){ jsnode_t *n = node(N_NUM); n->num = t->n; adv(); return n; }
    if(t->t == T_STRING){ jsnode_t *n = node(N_STR); n->str = t->s; adv(); return n; }
    if(t->t == T_REGEX){ jsnode_t *n = node(N_REGEX); n->str = t->s; n->flags = t->flags; adv(); return n; }
    if(t->t == T_ID){
        if(strcmp(t->s, "true") == 0){ adv(); return node(N_TRUE); }
        if(strcmp(t->s, "false") == 0){ adv(); return node(N_FALSE); }
        if(strcmp(t->s, "null") == 0){ adv(); return node(N_NULL); }
        if(strcmp(t->s, "undefined") == 0){ adv(); return node(N_UNDEF); }
        if(strcmp(t->s, "this") == 0){ adv(); return node(N_THIS); }
        if(strcmp(t->s, "super") == 0){ adv(); return node(N_SUPER); }
        if(strcmp(t->s, "function") == 0){
            adv();
            const char *name = NULL;
            if(toks[ti].t == T_ID){ name = toks[ti].s; adv(); }
            return function_rest(name);
        }
        if(ti + 1 < ntok && toks[ti+1].t == T_PUNCT && strcmp(toks[ti+1].s, "=>") == 0){
            jsnode_t *f = node(N_FUNC);
            jsnode_t *p = node(N_IDENT);
            p->str = t->s;
            f->list = p;
            adv(); adv();
            return arrow_body(f);
        }
        jsnode_t *n = node(N_IDENT);
        n->str = t->s;
        adv();
        return n;
    }
    if(tis("(")){
        int depth = 0, k = ti;
        for(; k < ntok; k++){
            if(toks[k].t == T_PUNCT && strcmp(toks[k].s, "(") == 0) depth++;
            if(toks[k].t == T_PUNCT && strcmp(toks[k].s, ")") == 0){ depth--; if(depth == 0) break; }
        }
        if(k + 1 < ntok && toks[k+1].t == T_PUNCT && strcmp(toks[k+1].s, "=>") == 0){
            jsnode_t *f = node(N_FUNC);
            adv();
            jsnode_t *last = NULL;
            while(!tis(")") && toks[ti].t != T_EOF){
                jsnode_t *p = node(N_IDENT);
                p->str = toks[ti].s;
                adv();
                if(last) last->next = p; else f->list = p;
                last = p;
                if(tis(",")) adv();
            }
            expect(")");
            expect("=>");
            return arrow_body(f);
        }
        adv();
        jsnode_t *e = expression();
        expect(")");
        return e;
    }
    if(tis("[")){
        adv();
        jsnode_t *n = node(N_ARRAY);
        jsnode_t *last = NULL;
        while(!tis("]") && toks[ti].t != T_EOF && !STOP()){
            jsnode_t *e;
            if(tis("...")){ adv(); e = node(N_SPREAD); e->a = assignment(); }
            else e = assignment();
            if(last) last->next = e; else n->list = e;
            last = e;
            if(tis(",")) adv(); else break;
        }
        expect("]");
        return n;
    }
    if(tis("{")){
        adv();
        jsnode_t *n = node(N_OBJECT);
        jsnode_t *last = NULL;
        while(!tis("}") && toks[ti].t != T_EOF && !STOP()){
            jsnode_t *p = node(N_IDENT);
            if((tkw("get") || tkw("set")) && ti + 2 < ntok && toks[ti+1].t == T_ID &&
               toks[ti+2].t == T_PUNCT && strcmp(toks[ti+2].s, "(") == 0){
                p->op = tkw("get") ? 1 : 2;       /* an accessor */
                adv();
            }
            if(toks[ti].t == T_ID || toks[ti].t == T_STRING){ p->str = toks[ti].s; }
            else if(toks[ti].t == T_NUMBER){
                char b[40];
                kfmt_double(b, sizeof b, toks[ti].n);
                p->str = jstrdup(b, (u32)strlen(b));
            } else { fail("bad property name"); break; }
            adv();
            if(tis("(")){                    /* a method shorthand: name(args) { ... } */
                p->a = function_rest(p->str);
            } else if(tis(":")){
                adv();
                p->a = assignment();
            } else {                         /* a property shorthand: { x } means { x: x } */
                jsnode_t *id = node(N_IDENT);
                id->str = p->str;
                p->a = id;
            }
            if(last) last->next = p; else n->list = p;
            last = p;
            if(tis(",")) adv(); else break;
        }
        expect("}");
        return n;
    }
    fail("unexpected token");
    adv();
    return node(N_UNDEF);
}

static jsnode_t *call_args(jsnode_t *callee){
    jsnode_t *c = node(N_CALL);
    c->a = callee;
    jsnode_t *last = NULL;
    while(!tis(")") && toks[ti].t != T_EOF && !STOP()){
        jsnode_t *arg;
        if(tis("...")){ adv(); arg = node(N_SPREAD); arg->a = assignment(); }
        else arg = assignment();
        if(last) last->next = arg; else c->list = arg;
        last = arg;
        if(tis(",")) adv(); else break;
    }
    expect(")");
    return c;
}

static jsnode_t *postfix(void){
    jsnode_t *e;
    if(tkw("new")){
        adv();
        jsnode_t *callee = primary();
        for(;;){
            if(tis(".")){
                adv();
                jsnode_t *m = node(N_MEMBER);
                m->a = callee;
                m->str = toks[ti].s;
                adv();
                callee = m;
            } else if(tis("[")){
                adv();
                jsnode_t *m = node(N_MEMBER);
                m->a = callee;
                m->b = expression();
                m->computed = 1;
                expect("]");
                callee = m;
            } else break;
        }
        jsnode_t *n = node(N_NEW);
        n->a = callee;
        if(tis("(")){ adv(); jsnode_t *c = call_args(NULL); n->list = c->list; }
        e = n;
    } else e = primary();
    for(;;){
        if(tis(".")){
            adv();
            jsnode_t *m = node(N_MEMBER);
            m->a = e;
            if(toks[ti].t != T_ID){ fail("expected a name after ."); break; }
            m->str = toks[ti].s;
            adv();
            e = m;
        } else if(tis("[")){
            adv();
            jsnode_t *m = node(N_MEMBER);
            m->a = e;
            m->b = expression();
            m->computed = 1;
            expect("]");
            e = m;
        } else if(tis("(")){
            adv();
            e = call_args(e);
        } else if(tis("?.")){               /* optional chaining */
            adv();
            if(tis("(")){                    /* fn?.() */
                adv();
                jsnode_t *c = call_args(e);
                c->op = 1;
                e = c;
            } else if(tis("[")){             /* a?.[i] */
                adv();
                jsnode_t *m = node(N_MEMBER);
                m->a = e;
                m->b = expression();
                m->computed = 1;
                m->op = 1;
                expect("]");
                e = m;
            } else {                         /* a?.b */
                jsnode_t *m = node(N_MEMBER);
                m->a = e;
                if(toks[ti].t != T_ID){ fail("expected a name after ?."); break; }
                m->str = toks[ti].s;
                m->op = 1;
                adv();
                e = m;
            }
        } else if(tis("++") || tis("--")){
            /* x++ and x-- act as x += 1 and x -= 1 (the new value is the result) */
            jsnode_t *a = node(N_ASSIGN);
            a->op = tis("++") ? OP_ADDSET : OP_SUBSET;
            a->a = e;
            jsnode_t *one = node(N_NUM);
            one->num = 1;
            a->b = one;
            adv();
            e = a;
        } else break;
    }
    return e;
}

static jsnode_t *unary(void){
    if(tis("++") || tis("--")){
        jsnode_t *a = node(N_ASSIGN);
        a->op = tis("++") ? OP_ADDSET : OP_SUBSET;
        adv();
        a->a = unary();
        jsnode_t *one = node(N_NUM);
        one->num = 1;
        a->b = one;
        return a;
    }
    if(tis("!") || tis("-") || tis("+") || tkw("typeof") || tkw("delete") || tkw("void")){
        int op = tkw("typeof") ? OP_TYPEOF : tkw("delete") ? OP_DELETE : tkw("void") ? OP_VOID :
                 tis("!") ? OP_NOT : tis("-") ? OP_NEG : OP_POS;
        adv();
        jsnode_t *n = node(N_UNARY);
        n->op = op;
        n->a = unary();
        return n;
    }
    return postfix();
}

static jsnode_t *binary(int minp){
    jsnode_t *left = unary();
    for(;;){
        int p;
        const char *op = NULL;
        if(toks[ti].t == T_PUNCT){ op = toks[ti].s; p = prec(op); }
        else if(tkw("instanceof") || tkw("in")){ op = toks[ti].s; p = 4; }
        else break;
        if(p == 0 || p < minp) break;
        adv();
        int rassoc = strcmp(op, "**") == 0;       /* exponentiation is right-associative */
        jsnode_t *right = binary(rassoc ? p : p + 1);
        jsnode_t *n = node(p <= 2 ? N_LOGIC : N_BINARY);
        n->op = binop(op);
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static jsnode_t *conditional(void){
    jsnode_t *c = binary(1);
    if(tis("?")){
        adv();
        jsnode_t *n = node(N_COND);
        n->a = c;
        n->b = assignment();
        expect(":");
        n->c = assignment();
        return n;
    }
    return c;
}

static jsnode_t *assignment(void){
    jsnode_t *left = conditional();
    int op = OP_NONE;
    if(tis("=")) op = OP_SET;
    else if(tis("+=")) op = OP_ADDSET;
    else if(tis("-=")) op = OP_SUBSET;
    if(op != OP_NONE){
        adv();
        jsnode_t *n = node(N_ASSIGN);
        n->op = op;
        n->a = left;
        n->b = assignment();
        return n;
    }
    return left;
}

static jsnode_t *expression(void){
    return assignment();
}

/* A destructuring pattern: [a, b, ...rest] or {a, b: c, ...rest}.
 * Array patterns are N_ARRAY of N_IDENT / N_SPREAD(N_IDENT); object patterns are
 * N_OBJECT of N_IDENT (str = source key, a = target name) / N_SPREAD(N_IDENT). */
static jsnode_t *parse_pattern(void){
    jsnode_t *n, *last = NULL;
    if(tis("[")){
        adv();
        n = node(N_ARRAY);
        while(!tis("]") && toks[ti].t != T_EOF && !STOP()){
            jsnode_t *it;
            if(tis("...")){
                adv();
                it = node(N_SPREAD);
                jsnode_t *id = node(N_IDENT); id->str = toks[ti].s; adv();
                it->a = id;
            } else {
                it = node(N_IDENT); it->str = toks[ti].s; adv();
            }
            if(last) last->next = it; else n->list = it;
            last = it;
            if(tis(",")) adv(); else break;
        }
        expect("]");
        return n;
    }
    adv();                                  /* { */
    n = node(N_OBJECT);
    while(!tis("}") && toks[ti].t != T_EOF && !STOP()){
        jsnode_t *it;
        if(tis("...")){
            adv();
            it = node(N_SPREAD);
            jsnode_t *id = node(N_IDENT); id->str = toks[ti].s; adv();
            it->a = id;
        } else {
            it = node(N_IDENT); it->str = toks[ti].s; adv();   /* source key */
            if(tis(":")){ adv(); jsnode_t *tgt = node(N_IDENT); tgt->str = toks[ti].s; adv(); it->a = tgt; }
        }
        if(last) last->next = it; else n->list = it;
        last = it;
        if(tis(",")) adv(); else break;
    }
    expect("}");
    return n;
}

static jsnode_t *var_decl(void){
    jsnode_t *v = node(N_VAR);
    adv();
    jsnode_t *last = NULL;
    for(;;){
        jsnode_t *d = node(N_DECL);
        if(tis("[") || tis("{")){           /* destructuring: [a,b] = … or {a,b} = … */
            d->b = parse_pattern();
            if(tis("=")){ adv(); d->a = assignment(); }
        } else {
            if(toks[ti].t != T_ID){ fail("expected a name"); break; }
            d->str = toks[ti].s;
            adv();
            if(tis("=")){ adv(); d->a = assignment(); }
        }
        if(last) last->next = d; else v->list = d;
        last = d;
        if(tis(",")) adv(); else break;
    }
    return v;
}

static jsnode_t *block_body(void){
    jsnode_t *b = node(N_BLOCK);
    expect("{");
    jsnode_t *last = NULL;
    while(!tis("}") && toks[ti].t != T_EOF && !STOP()){
        jsnode_t *s = statement();
        if(last) last->next = s; else b->list = s;
        last = s;
    }
    expect("}");
    return b;
}

static jsnode_t *class_decl(void){
    jsnode_t *c = node(N_CLASS);
    adv();                                       /* class */
    if(toks[ti].t == T_ID){ c->str = toks[ti].s; adv(); }
    if(tkw("extends")){ adv(); c->a = postfix(); }
    expect("{");
    jsnode_t *last = NULL;
    while(!tis("}") && toks[ti].t != T_EOF && !STOP()){
        if(tis(";")){ adv(); continue; }
        if(toks[ti].t != T_ID){ fail("bad class member"); break; }
        jsnode_t *m = node(N_FUNCDECL);
        if((tkw("get") || tkw("set")) && ti + 2 < ntok && toks[ti+1].t == T_ID &&
           toks[ti+2].t == T_PUNCT && strcmp(toks[ti+2].s, "(") == 0){
            m->op = tkw("get") ? 1 : 2;
            adv();
        }
        m->str = toks[ti].s;
        adv();
        m->a = function_rest(m->str);
        if(last) last->next = m; else c->list = m;
        last = m;
    }
    expect("}");
    return c;
}

static jsnode_t *statement(void){
    if(tis("{")) return block_body();
    if(tis(";")){ adv(); return node(N_EMPTY); }
    if(tkw("var") || tkw("let") || tkw("const")){
        if(ti + 2 < ntok && toks[ti+1].t == T_ID && toks[ti+2].t == T_ID &&
           (strcmp(toks[ti+2].s, "of") == 0 || strcmp(toks[ti+2].s, "in") == 0)){
            /* for-in / for-of is handled by the for statement */
        }
        jsnode_t *v = var_decl();
        if(tis(";")) adv();
        return v;
    }
    if(tkw("class")) return class_decl();
    if(tkw("function") && ti + 1 < ntok && toks[ti+1].t == T_ID){
        adv();
        const char *name = toks[ti].s;
        adv();
        jsnode_t *d = node(N_FUNCDECL);
        d->str = name;
        d->a = function_rest(name);
        return d;
    }
    if(tkw("if")){
        adv();
        jsnode_t *n = node(N_IF);
        expect("(");
        n->a = expression();
        expect(")");
        n->b = statement();
        if(tkw("else")){ adv(); n->c = statement(); }
        return n;
    }
    if(tkw("while")){
        adv();
        jsnode_t *n = node(N_WHILE);
        expect("(");
        n->a = expression();
        expect(")");
        n->b = statement();
        return n;
    }
    if(tkw("do")){
        adv();
        jsnode_t *n = node(N_DO);
        n->b = statement();
        if(tkw("while")) adv();
        expect("(");
        n->a = expression();
        expect(")");
        if(tis(";")) adv();
        return n;
    }
    if(tkw("for")){
        adv();
        expect("(");
        if((tkw("var") || tkw("let") || tkw("const")) && ti + 2 < ntok && toks[ti+1].t == T_ID &&
           toks[ti+2].t == T_ID && (strcmp(toks[ti+2].s, "of") == 0 || strcmp(toks[ti+2].s, "in") == 0)){
            jsnode_t *n = node(N_FORIN);
            adv();
            n->str = toks[ti].s;
            adv();
            n->op = strcmp(toks[ti].s, "of") == 0 ? 1 : 0;
            adv();
            n->a = expression();
            expect(")");
            n->d = statement();
            return n;
        }
        jsnode_t *n = node(N_FOR);
        if(tkw("var") || tkw("let") || tkw("const")) n->a = var_decl();
        else if(!tis(";")){ jsnode_t *e = node(N_EXPR); e->a = expression(); n->a = e; }
        expect(";");
        if(!tis(";")) n->b = expression();
        expect(";");
        if(!tis(")")) n->c = expression();
        expect(")");
        n->d = statement();
        return n;
    }
    if(tkw("switch")){
        adv();
        jsnode_t *n = node(N_SWITCH);
        expect("(");
        n->a = expression();
        expect(")");
        expect("{");
        jsnode_t *last = NULL;
        while(!tis("}") && toks[ti].t != T_EOF && !STOP()){
            jsnode_t *c = node(N_CASE);
            if(tkw("case")){ adv(); c->a = expression(); }
            else if(tkw("default")) adv();
            else { fail("expected case"); break; }
            expect(":");
            jsnode_t *ll = NULL;
            while(!tkw("case") && !tkw("default") && !tis("}") && toks[ti].t != T_EOF && !STOP()){
                jsnode_t *s = statement();
                if(ll) ll->next = s; else c->list = s;
                ll = s;
            }
            if(last) last->next = c; else n->list = c;
            last = c;
        }
        expect("}");
        return n;
    }
    if(tkw("try")){
        adv();
        jsnode_t *n = node(N_TRY);
        n->a = block_body();
        if(tkw("catch")){
            adv();
            if(tis("(")){ adv(); n->str = toks[ti].s; adv(); expect(")"); }
            n->b = block_body();
        }
        if(tkw("finally")){ adv(); n->c = block_body(); }
        return n;
    }
    if(tkw("throw")){
        adv();
        jsnode_t *n = node(N_THROW);
        n->a = expression();
        if(tis(";")) adv();
        return n;
    }
    if(tkw("return")){
        adv();
        jsnode_t *n = node(N_RETURN);
        if(!tis(";") && !tis("}")) n->a = expression();
        if(tis(";")) adv();
        return n;
    }
    if(tkw("break")){ adv(); if(tis(";")) adv(); return node(N_BREAK); }
    if(tkw("continue")){ adv(); if(tis(";")) adv(); return node(N_CONTINUE); }
    jsnode_t *n = node(N_EXPR);
    n->a = expression();
    if(tis(";")) adv();
    return n;
}

/* ---- environments ---- */
static env_t *new_env(env_t *parent){
    env_t *e = jalloc(sizeof(env_t));
    e->parent = parent;
    e->first = NULL;
    return e;
}

static void env_define(env_t *e, const char *name, js_val_t v){
    for(binding_t *b = e->first; b; b = b->next)
        if(strcmp(b->name, name) == 0){ b->v = v; return; }
    binding_t *b = jalloc(sizeof(binding_t));
    b->name = name;
    b->v = v;
    b->next = e->first;
    e->first = b;
}

static binding_t *env_find(env_t *e, const char *name){
    for(; e; e = e->parent)
        for(binding_t *b = e->first; b; b = b->next)
            if(strcmp(b->name, name) == 0) return b;
    return NULL;
}

/* ---- evaluation ---- */
static int ctl = 0;                    /* 0 none, 1 return, 2 break, 3 continue */
static js_val_t ret_val;
static int depth = 0;
static long steps = 0;

static js_val_t eval(jsnode_t *n, env_t *env);

/* Evaluates a call/array argument list into out[], expanding ...spread elements.
 * Returns the number of values produced (capped at max). */
static int eval_args(jsnode_t *list, env_t *env, js_val_t *out, int max){
    int argc = 0;
    for(jsnode_t *e = list; e && argc < max; e = e->next){
        if(e->k == N_SPREAD){
            js_val_t s = eval(e->a, env);
            if(s.t == JS_OBJ && s.o->is_array)
                for(int i = 0; i < s.o->nitems && argc < max; i++) out[argc++] = s.o->items[i];
        } else out[argc++] = eval(e, env);
    }
    return argc;
}
static void exec(jsnode_t *n, env_t *env);
static js_val_t call_value(js_val_t fn, js_val_t self, int argc, js_val_t *argv);

static int step(void){
    if(++steps > MAX_STEPS){ fail("script ran for too long"); return 0; }
    return !STOP();
}

static js_val_t read_ident(env_t *env, const char *name){
    binding_t *b = env_find(env, name);
    if(b) return b->v;
    if(global_obj){
        js_val_t out = js_undef();
        if(has_prop(global_obj, name)) return js_get(global_obj, name);
        if(global_obj->hget && global_obj->hget(global_obj, name, &out)) return out;
    }
    char *msg = jalloc(96);
    u32 k = 0;
    for(const char *q = name; *q && k < 60; q++) msg[k++] = *q;
    const char *tail = " is not defined";
    for(const char *q = tail; *q; q++) msg[k++] = *q;
    msg[k] = 0;
    fail(msg);
    return js_undef();
}

static int same(js_val_t a, js_val_t b){
    if(a.t != b.t) return 0;
    switch(a.t){
        case JS_UNDEF: case JS_NULL: return 1;
        case JS_BOOL: return a.b == b.b;
        case JS_NUM: return a.n == b.n;
        case JS_STR: return strcmp(a.s, b.s) == 0;
        case JS_OBJ: return a.o == b.o;
    }
    return 0;
}

static int loose_eq(js_val_t a, js_val_t b){
    if(a.t == b.t) return same(a, b);
    if((a.t == JS_NULL || a.t == JS_UNDEF) && (b.t == JS_NULL || b.t == JS_UNDEF)) return 1;
    if(a.t == JS_NULL || a.t == JS_UNDEF || b.t == JS_NULL || b.t == JS_UNDEF) return 0;
    if(a.t == JS_OBJ || b.t == JS_OBJ) return 0;
    return js_to_num(a) == js_to_num(b);
}

static js_val_t concat(js_val_t a, js_val_t b){
    const char *x = js_to_str(a), *y = js_to_str(b);
    u32 lx = (u32)strlen(x), ly = (u32)strlen(y);
    if(lx + ly > 16384){ fail("string too long"); return js_str(""); }
    char *r = jalloc(lx + ly + 1);
    memcpy(r, x, lx);
    memcpy(r + lx, y, ly);
    r[lx + ly] = 0;
    return js_str(r);
}

static js_obj_t *prototype_of(js_obj_t *ctor){
    js_val_t p = js_get(ctor, "prototype");
    return p.t == JS_OBJ ? p.o : NULL;
}

static js_val_t binop_eval(int op, js_val_t a, js_val_t b){
    switch(op){
        case OP_ADD:
            if(a.t == JS_STR || b.t == JS_STR || a.t == JS_OBJ || b.t == JS_OBJ) return concat(a, b);
            return js_num(js_to_num(a) + js_to_num(b));
        case OP_SUB: return js_num(js_to_num(a) - js_to_num(b));
        case OP_MUL: return js_num(js_to_num(a) * js_to_num(b));
        case OP_DIV: return js_num(js_to_num(a) / js_to_num(b));
        case OP_MOD: return js_num(kfmod(js_to_num(a), js_to_num(b)));
        case OP_POW: return js_num(kpow(js_to_num(a), js_to_num(b)));
        case OP_EQ: return js_bool(loose_eq(a, b));
        case OP_NE: return js_bool(!loose_eq(a, b));
        case OP_SEQ: return js_bool(same(a, b));
        case OP_SNE: return js_bool(!same(a, b));
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            int r;
            if(a.t == JS_STR && b.t == JS_STR){
                int c = strcmp(a.s, b.s);
                r = op == OP_LT ? c < 0 : op == OP_GT ? c > 0 : op == OP_LE ? c <= 0 : c >= 0;
            } else {
                double x = js_to_num(a), y = js_to_num(b);
                r = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : x >= y;
            }
            return js_bool(r);
        }
        case OP_INSTANCEOF: {
            if(a.t != JS_OBJ || b.t != JS_OBJ) return js_bool(0);
            js_obj_t *p = prototype_of(b.o);
            for(js_obj_t *x = a.o->proto; x && p; x = x->proto) if(x == p) return js_bool(1);
            return js_bool(0);
        }
        case OP_IN: {
            if(b.t != JS_OBJ) { fail("'in' needs an object"); return js_bool(0); }
            return js_bool(has_prop(b.o, js_to_str(a)));
        }
    }
    return js_undef();
}

static js_val_t get_member(js_val_t obj, const char *name){
    if(obj.t == JS_STR){
        if(strcmp(name, "length") == 0) return js_num((double)strlen(obj.s));
        return js_get(string_proto, name);
    }
    if(obj.t == JS_OBJ) return js_get(obj.o, name);
    if(obj.t == JS_NUM) return js_get(number_proto, name);
    if(obj.t == JS_NULL || obj.t == JS_UNDEF){
        char m[80] = "cannot read \"";
        u32 k = (u32)strlen(m);
        for(const char *q = name; *q && k < 60; q++) m[k++] = *q;
        const char *tail = "\" of null or undefined";
        for(const char *q = tail; *q; q++) m[k++] = *q;
        m[k] = 0;
        char *msg = jalloc(k + 1);
        memcpy(msg, m, k + 1);
        fail(msg);
    }
    return js_undef();
}

static const char *key_of(jsnode_t *m, env_t *env){
    if(!m->computed) return m->str;
    js_val_t k = eval(m->b, env);
    return js_to_str(k);
}

static js_val_t make_fn(jsnode_t *f, env_t *env){
    js_obj_t *o = new_obj();
    o->is_func = 1;
    o->proto = object_proto;
    o->fdecl = f;
    o->closure = env;
    return js_objv(o);
}

static js_val_t construct(js_val_t ctor, int argc, js_val_t *argv){
    if(!js_callable(ctor)){ fail("not a constructor"); return js_undef(); }
    js_obj_t *obj = new_obj();
    obj->proto = prototype_of(ctor.o);
    if(!obj->proto) obj->proto = object_proto;
    js_val_t self = js_objv(obj);
    js_val_t r = call_value(ctor, self, argc, argv);
    if(r.t == JS_OBJ) return r;
    return self;
}

/* Copies source props not named in the pattern into rest, oldest-first so the
 * resulting key order matches the source object. */
static void obj_rest_copy(js_obj_t *rest, jsnode_t *pat, prop_t *p){
    if(!p) return;
    obj_rest_copy(rest, pat, p->next);
    int used = 0;
    for(jsnode_t *u = pat->list; u && !used; u = u->next)
        if(u->k != N_SPREAD && strcmp(u->str, p->k) == 0) used = 1;
    if(!used) js_set(rest, p->k, p->v);
}

/* Binds a destructuring pattern's targets from value v into env. */
static void bind_pattern(jsnode_t *pat, js_val_t v, env_t *env){
    if(pat->k == N_ARRAY){
        int i = 0;
        for(jsnode_t *it = pat->list; it; it = it->next, i++){
            if(it->k == N_SPREAD){
                js_obj_t *r = new_array();
                if(v.t == JS_OBJ && v.o->is_array)
                    for(int j = i; j < v.o->nitems; j++) arr_push(r, v.o->items[j]);
                env_define(env, it->a->str, js_objv(r));
                break;
            }
            js_val_t item = js_undef();
            if(v.t == JS_OBJ && v.o->is_array && i < v.o->nitems) item = v.o->items[i];
            env_define(env, it->str, item);
        }
    } else {                                 /* object pattern */
        for(jsnode_t *it = pat->list; it; it = it->next){
            if(it->k == N_SPREAD){           /* ...rest: remaining own keys as a new object */
                js_obj_t *rest = js_new_object();
                if(v.t == JS_OBJ) obj_rest_copy(rest, pat, v.o->props);
                env_define(env, it->a->str, js_objv(rest));
                continue;
            }
            js_val_t val = (v.t == JS_OBJ) ? get_member(v, it->str) : js_undef();
            env_define(env, it->a ? it->a->str : it->str, val);
        }
    }
}

static js_val_t eval(jsnode_t *n, env_t *env){
    if(!n || STOP()) return js_undef();
    if(!step()) return js_undef();
    switch(n->k){
        case N_NUM: return js_num(n->num);
        case N_STR: return js_str(n->str);
        case N_TRUE: return js_bool(1);
        case N_FALSE: return js_bool(0);
        case N_NULL: return js_null();
        case N_UNDEF: return js_undef();
        case N_THIS: return read_ident(env, "this");
        case N_IDENT: return read_ident(env, n->str);
        case N_REGEX: {
            js_obj_t *r = new_obj();
            r->is_regex = 1;
            r->proto = regex_proto;
            r->re = jalloc(sizeof(regex_data_t));
            r->re->pat = n->str;
            r->re->plen = (u32)strlen(n->str);
            r->re->icase = strchr(n->flags, 'i') != NULL;
            r->re->global = strchr(n->flags, 'g') != NULL;
            js_set(r, "source", js_str(n->str));
            js_set(r, "flags", js_str(n->flags));
            js_set(r, "global", js_bool(r->re->global));
            return js_objv(r);
        }
        case N_ARRAY: {
            js_obj_t *a = new_array();
            for(jsnode_t *e = n->list; e; e = e->next){
                if(e->k == N_SPREAD){
                    js_val_t s = eval(e->a, env);
                    if(s.t == JS_OBJ && s.o->is_array)
                        for(int i = 0; i < s.o->nitems; i++) arr_push(a, s.o->items[i]);
                } else arr_push(a, eval(e, env));
            }
            return js_objv(a);
        }
        case N_OBJECT: {
            js_obj_t *o = js_new_object();
            for(jsnode_t *p = n->list; p; p = p->next){
                if(p->op == 1 || p->op == 2){
                    js_val_t fv = make_fn(p->a, env);
                    fv.o->home = o;
                    js_set(o, p->str, js_undef());
                    prop_t *pr = find_own(o, p->str);
                    if(p->op == 1) pr->getter = fv.o; else pr->setter = fv.o;
                    continue;
                }
                js_val_t v = eval(p->a, env);
                if(v.t == JS_OBJ && v.o->is_func && p->a->k == N_FUNC && !p->a->arrow) v.o->home = o;
                js_set(o, p->str, v);
            }
            return js_objv(o);
        }
        case N_FUNC: return make_fn(n, env);
        case N_MEMBER: {
            if(n->a->k == N_SUPER){
                binding_t *h = env_find(env, "%home");
                if(!h || h->v.t != JS_OBJ || !h->v.o->proto){ fail("super is not available here"); return js_undef(); }
                return js_get(h->v.o->proto, key_of(n, env));
            }
            js_val_t obj = eval(n->a, env);
            if(n->op == 1 && (obj.t == JS_NULL || obj.t == JS_UNDEF)) return js_undef();
            return get_member(obj, key_of(n, env));
        }
        case N_CALL: {
            if(n->a && n->a->k == N_SUPER){
                /* super(args): the superclass constructor runs on this */
                binding_t *f = env_find(env, "%fn");
                binding_t *th = env_find(env, "this");
                js_val_t args[32];
                int argc = eval_args(n->list, env, args, 32);
                if(!f || !th || f->v.t != JS_OBJ || !f->v.o->parent){ fail("no superclass"); return js_undef(); }
                return call_value(js_objv(f->v.o->parent), th->v, argc, args);
            }
            js_val_t self = js_undef();
            js_val_t fn;
            if(n->a->k == N_MEMBER && n->a->a->k == N_SUPER){
                binding_t *h = env_find(env, "%home");
                binding_t *th = env_find(env, "this");
                if(!h || h->v.t != JS_OBJ || !h->v.o->proto || !th){ fail("super is not available here"); return js_undef(); }
                self = th->v;
                fn = js_get(h->v.o->proto, key_of(n->a, env));
            } else if(n->a->k == N_MEMBER){
                self = eval(n->a->a, env);
                fn = get_member(self, key_of(n->a, env));
            } else fn = eval(n->a, env);
            if(n->op == 1 && (fn.t == JS_NULL || fn.t == JS_UNDEF)) return js_undef();  /* fn?.() */
            js_val_t args[32];
            int argc = eval_args(n->list, env, args, 32);
            if(STOP()) return js_undef();
            if(!js_callable(fn)){ fail("not a function"); return js_undef(); }
            return call_value(fn, self, argc, args);
        }
        case N_NEW: {
            js_val_t ctor = eval(n->a, env);
            js_val_t args[32];
            int argc = eval_args(n->list, env, args, 32);
            if(STOP()) return js_undef();
            return construct(ctor, argc, args);
        }
        case N_UNARY: {
            if(n->op == OP_DELETE){
                jsnode_t *t = n->a;
                if(t->k == N_MEMBER){
                    js_val_t obj = eval(t->a, env);
                    if(obj.t == JS_OBJ){
                        const char *k = key_of(t, env);
                        if(obj.o->is_array){
                            int idx = as_index(k);
                            if(idx >= 0 && idx < obj.o->nitems){ obj.o->items[idx] = js_undef(); return js_bool(1); }
                        }
                        prop_remove(obj.o, k);
                    }
                }
                return js_bool(1);
            }
            js_val_t v = eval(n->a, env);
            switch(n->op){
                case OP_VOID: (void)v; return js_undef();
                case OP_NOT: return js_bool(!js_truthy(v));
                case OP_NEG: return js_num(-js_to_num(v));
                case OP_POS: return js_num(js_to_num(v));
                case OP_TYPEOF: {
                    const char *t = v.t == JS_UNDEF ? "undefined" : v.t == JS_NULL ? "object" :
                                    v.t == JS_BOOL ? "boolean" : v.t == JS_NUM ? "number" :
                                    v.t == JS_STR ? "string" : (v.o->is_func ? "function" : "object");
                    return js_str(t);
                }
            }
            return js_undef();
        }
        case N_BINARY: {
            js_val_t a = eval(n->a, env);
            js_val_t b = eval(n->b, env);
            return binop_eval(n->op, a, b);
        }
        case N_LOGIC: {
            js_val_t a = eval(n->a, env);
            if(n->op == OP_NULLISH) return (a.t == JS_NULL || a.t == JS_UNDEF) ? eval(n->b, env) : a;
            if(n->op == OP_AND) return js_truthy(a) ? eval(n->b, env) : a;
            return js_truthy(a) ? a : eval(n->b, env);
        }
        case N_COND: return js_truthy(eval(n->a, env)) ? eval(n->b, env) : eval(n->c, env);
        case N_ASSIGN: {
            js_val_t v = eval(n->b, env);
            jsnode_t *t = n->a;
            if(n->op == OP_ADDSET || n->op == OP_SUBSET){
                js_val_t cur = eval(t, env);
                v = binop_eval(n->op == OP_ADDSET ? OP_ADD : OP_SUB, cur, v);
            }
            if(t->k == N_IDENT){
                binding_t *b = env_find(env, t->str);
                if(b) b->v = v;
                else if(global_obj) js_set(global_obj, t->str, v);
            } else if(t->k == N_MEMBER){
                js_val_t obj = eval(t->a, env);
                if(obj.t == JS_OBJ) obj_set(obj.o, key_of(t, env), v);
                else fail("cannot set a property of null or undefined");
            } else fail("bad assignment target");
            return v;
        }
        case N_CLASS: {
            js_obj_t *ctor = new_obj();
            ctor->is_func = 1;
            ctor->is_class = 1;
            ctor->closure = env;
            ctor->proto = object_proto;
            js_obj_t *proto = js_new_object();
            js_set(ctor, "prototype", js_objv(proto));
            js_set(proto, "constructor", js_objv(ctor));
            if(n->a){
                js_val_t sup = eval(n->a, env);
                if(sup.t != JS_OBJ || !sup.o->is_func){ fail("extends needs a class"); return js_undef(); }
                ctor->parent = sup.o;
                js_obj_t *sp = prototype_of(sup.o);
                if(sp) proto->proto = sp;
            } else proto->proto = object_proto;
            for(jsnode_t *m = n->list; m; m = m->next){
                js_val_t fv = make_fn(m->a, env);
                fv.o->home = proto;
                if(m->op == 1 || m->op == 2){
                    js_set(proto, m->str, js_undef());
                    prop_t *pr = find_own(proto, m->str);
                    if(m->op == 1) pr->getter = fv.o; else pr->setter = fv.o;
                } else if(strcmp(m->str, "constructor") == 0){
                    ctor->fdecl = m->a;
                } else {
                    js_set(proto, m->str, fv);
                }
            }
            if(!ctor->fdecl) ctor->default_ctor = 1;
            if(n->str) env_define(env, n->str, js_objv(ctor));
            return js_objv(ctor);
        }
        default:
            fail("unsupported expression");
            return js_undef();
    }
}

static js_val_t call_value(js_val_t fn, js_val_t self, int argc, js_val_t *argv){
    js_obj_t *o = (fn.t == JS_OBJ) ? fn.o : NULL;
    if(!o || !o->is_func) return js_undef();   /* calling a non-function used to fault at o->native */
    if(o->native) return o->native(self, argc, argv, o->ctx);
    if(o->default_ctor){
        /* a class without a constructor passes its arguments to the superclass */
        if(o->parent) return call_value(js_objv(o->parent), self, argc, argv);
        return js_undef();
    }
    if(!o->fdecl) return js_undef();
    if(depth >= MAX_DEPTH){ fail("too much recursion"); return js_undef(); }
    jsnode_t *f = o->fdecl;
    env_t *env = new_env(o->closure);
    if(!f->arrow){
        /* non-strict: a plain call (no receiver) sees the global object as this */
        if(self.t == JS_UNDEF || self.t == JS_NULL) self = js_objv(global_obj);
        env_define(env, "this", self);
        env_define(env, "%fn", js_objv(o));
        if(o->home) env_define(env, "%home", js_objv(o->home));
    }
    int i = 0;
    for(jsnode_t *p = f->list; p; p = p->next, i++){
        if(p->op == 1){                  /* rest parameter: the remaining args as an array */
            js_obj_t *r = new_array();
            for(int k = i; k < argc; k++) arr_push(r, argv[k]);
            env_define(env, p->str, js_objv(r));
            break;
        }
        js_val_t v;
        if(i < argc && argv[i].t != JS_UNDEF) v = argv[i];
        else if(p->b) v = eval(p->b, env);       /* default value */
        else v = js_undef();
        env_define(env, p->str, v);
    }
    if(!f->arrow){
        js_obj_t *args = new_array();
        for(int k = 0; k < argc; k++) arr_push(args, argv[k]);
        env_define(env, "arguments", js_objv(args));
    }
    for(jsnode_t *s = f->a ? f->a->list : NULL; s; s = s->next)
        if(s->k == N_FUNCDECL) env_define(env, s->str, make_fn(s->a, env));
    depth++;
    int saved_ctl = ctl;
    ctl = 0;
    exec(f->a, env);
    depth--;
    js_val_t r = js_undef();
    if(ctl == 1) r = ret_val;
    ctl = saved_ctl;
    return r;
}

js_val_t js_call(js_val_t fn, js_val_t self, int argc, js_val_t *argv){
    if(!js_callable(fn)) return js_undef();
    return call_value(fn, self, argc, argv);
}

/* The keys of an object (or the indices of an array), as an array of strings. */
static js_obj_t *keys_of(js_val_t v){
    js_obj_t *out = new_array();
    if(v.t != JS_OBJ) return out;
    if(v.o->is_array){
        for(int i = 0; i < v.o->nitems; i++){
            char b[12]; int k = 0; int x = i;
            char t[12]; int tk = 0;
            do { t[tk++] = (char)('0' + x % 10); x /= 10; } while(x);
            while(tk) b[k++] = t[--tk];
            b[k] = 0;
            arr_push(out, js_str(jstrdup(b, (u32)k)));
        }
    }
    /* own properties were added newest first; list them in the order they were made */
    int n = 0;
    for(prop_t *p = v.o->props; p; p = p->next) n++;
    for(int i = n - 1; i >= 0; i--){
        prop_t *p = v.o->props;
        for(int k = 0; k < i; k++) p = p->next;
        arr_push(out, js_str(p->k));
    }
    return out;
}

static void exec_loop_body(jsnode_t *body, env_t *env, int *stop){
    exec(body, env);
    *stop = 0;
    if(ctl == 1) *stop = 1;
    else if(ctl == 2){ ctl = 0; *stop = 1; }
    else if(ctl == 3) ctl = 0;
}

static void exec(jsnode_t *n, env_t *env){
    if(!n || STOP()) return;
    if(!step()) return;
    switch(n->k){
        case N_BLOCK: {
            env_t *inner = new_env(env);
            for(jsnode_t *s = n->list; s; s = s->next)
                if(s->k == N_FUNCDECL) env_define(inner, s->str, make_fn(s->a, inner));
            for(jsnode_t *s = n->list; s && ctl == 0 && !STOP(); s = s->next){
                if(s->k == N_FUNCDECL) continue;
                exec(s, inner);
            }
            break;
        }
        case N_VAR:
            for(jsnode_t *d = n->list; d; d = d->next){
                if(d->b) bind_pattern(d->b, d->a ? eval(d->a, env) : js_undef(), env);
                else env_define(env, d->str, d->a ? eval(d->a, env) : js_undef());
            }
            break;
        case N_CLASS: {
            js_val_t c = eval(n, env);
            (void)c;
            break;
        }
        case N_FUNCDECL: break;
        case N_EXPR: eval(n->a, env); break;
        case N_IF:
            if(js_truthy(eval(n->a, env))) exec(n->b, env);
            else if(n->c) exec(n->c, env);
            break;
        case N_WHILE: {
            int stop = 0;
            while(!STOP() && !stop && js_truthy(eval(n->a, env))) exec_loop_body(n->b, env, &stop);
            break;
        }
        case N_DO: {
            int stop = 0;
            do {
                exec_loop_body(n->b, env, &stop);
                if(stop || STOP()) break;
            } while(js_truthy(eval(n->a, env)) && !STOP());
            break;
        }
        case N_FOR: {
            env_t *loop = new_env(env);
            if(n->a) exec(n->a, loop);
            int stop = 0;
            while(!STOP() && !stop && (!n->b || js_truthy(eval(n->b, loop)))){
                exec_loop_body(n->d, loop, &stop);
                if(stop || STOP()) break;
                if(n->c) eval(n->c, loop);
            }
            break;
        }
        case N_FORIN: {
            js_val_t coll = eval(n->a, env);
            js_obj_t *items = NULL;
            if(n->op == 1){
                if(coll.t == JS_OBJ && coll.o->is_array) items = coll.o;
                else if(coll.t == JS_STR){
                    items = new_array();
                    for(const char *p = coll.s; *p; p++){
                        char *c = jalloc(2); c[0] = *p; c[1] = 0;
                        arr_push(items, js_str(c));
                    }
                } else items = new_array();
            } else items = keys_of(coll);
            int stop = 0;
            for(int i = 0; i < items->nitems && !STOP() && !stop; i++){
                env_t *loop = new_env(env);
                env_define(loop, n->str, items->items[i]);
                exec_loop_body(n->d, loop, &stop);
            }
            break;
        }
        case N_SWITCH: {
            js_val_t v = eval(n->a, env);
            jsnode_t *start = NULL, *def = NULL;
            for(jsnode_t *c = n->list; c && !start; c = c->next){
                if(!c->a){ if(!def) def = c; continue; }
                if(same(v, eval(c->a, env))) start = c;
            }
            if(!start) start = def;
            env_t *inner = new_env(env);
            int running = 0;
            for(jsnode_t *c = start; c && !STOP(); c = c->next){
                if(!running && c == start) running = 1;
                if(!running) continue;
                for(jsnode_t *s = c->list; s && !STOP(); s = s->next){
                    exec(s, inner);
                    if(ctl == 2){ ctl = 0; return; }
                    if(ctl) return;
                }
            }
            break;
        }
        case N_TRY: {
            exec(n->a, env);
            if(n->b && (failed || thrown)){
                js_val_t err;
                if(thrown){ err = thrown_val; thrown = 0; }
                else { err = js_str(jstrdup(err_msg, (u32)strlen(err_msg))); failed = 0; }
                env_t *ce = new_env(env);
                if(n->str) env_define(ce, n->str, err);
                exec(n->b, ce);
            }
            if(n->c){
                int sf = failed, st = thrown;
                js_val_t sv = thrown_val;
                failed = 0; thrown = 0;
                int sctl = ctl; ctl = 0;
                exec(n->c, env);
                if(!failed && !thrown){
                    failed = sf; thrown = st; thrown_val = sv;
                    ctl = sctl;
                }
            }
            break;
        }
        case N_THROW:
            thrown_val = eval(n->a, env);
            thrown = 1;
            break;
        case N_RETURN:
            ret_val = n->a ? eval(n->a, env) : js_undef();
            ctl = 1;
            break;
        case N_BREAK: ctl = 2; break;
        case N_CONTINUE: ctl = 3; break;
        case N_EMPTY: break;
        default:
            eval(n, env);
            break;
    }
}

/* ---- built-ins ---- */

static js_val_t arg(int argc, js_val_t *argv, int i){
    return i < argc ? argv[i] : js_undef();
}

static js_val_t native_log(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    char out[160] = "";
    int k = 0;
    for(int i = 0; i < argc; i++){
        const char *s = js_to_str(argv[i]);
        for(const char *p = s; *p && k < 150; p++) out[k++] = *p;
        if(i + 1 < argc && k < 150) out[k++] = ' ';
    }
    out[k] = 0;
    memcpy(last_log, out, (u32)k + 1);
    return js_undef();
}

static js_val_t arr_push_native(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return js_undef();
    for(int i = 0; i < argc; i++) arr_push(self.o, argv[i]);
    return js_num(self.o->nitems);
}

static js_val_t arr_pop(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || self.o->nitems == 0) return js_undef();
    return self.o->items[--self.o->nitems];
}

static js_val_t arr_join(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return js_str("");
    const char *sep = argc ? js_to_str(argv[0]) : ",";
    char *out = jalloc(4096);
    u32 n = 0;
    for(int i = 0; i < self.o->nitems && n + 2 < 4096; i++){
        if(i){ for(const char *p = sep; *p && n + 2 < 4096; p++) out[n++] = *p; }
        const char *s = js_to_str(self.o->items[i]);
        for(; *s && n + 2 < 4096; s++) out[n++] = *s;
    }
    out[n] = 0;
    return js_str(out);
}

static js_val_t arr_index_of(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return js_num(-1);
    js_val_t x = arg(argc, argv, 0);
    for(int i = 0; i < self.o->nitems; i++) if(same(self.o->items[i], x)) return js_num(i);
    return js_num(-1);
}

static js_val_t arr_includes(js_val_t self, int argc, js_val_t *argv, void *ctx){
    js_val_t r = arr_index_of(self, argc, argv, ctx);
    return js_bool(r.n >= 0);
}

static js_val_t arr_slice(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    js_obj_t *out = new_array();
    if(self.t != JS_OBJ || !self.o->is_array) return js_objv(out);
    int len = self.o->nitems;
    int a = argc > 0 ? (int)js_to_num(argv[0]) : 0;
    int b = argc > 1 ? (int)js_to_num(argv[1]) : len;
    if(a < 0) a += len;
    if(b < 0) b += len;
    if(a < 0) a = 0;
    if(b > len) b = len;
    for(int i = a; i < b; i++) arr_push(out, self.o->items[i]);
    return js_objv(out);
}

static js_val_t arr_for_each(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_undef();
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        call_value(argv[0], js_undef(), 3, a);
    }
    return js_undef();
}

static js_val_t arr_map(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    js_obj_t *out = new_array();
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_objv(out);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        arr_push(out, call_value(argv[0], js_undef(), 3, a));
    }
    return js_objv(out);
}

static js_val_t arr_filter(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    js_obj_t *out = new_array();
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_objv(out);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(js_truthy(call_value(argv[0], js_undef(), 3, a))) arr_push(out, self.o->items[i]);
    }
    return js_objv(out);
}

static js_val_t arr_reduce(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_undef();
    int i = 0;
    js_val_t acc = argc > 1 ? argv[1] : (self.o->nitems ? self.o->items[i++] : js_undef());
    for(; i < self.o->nitems && !STOP(); i++){
        js_val_t a[4] = { acc, self.o->items[i], js_num(i), self };
        acc = call_value(argv[0], js_undef(), 4, a);
    }
    return acc;
}

static js_val_t str_upper(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    const char *s = js_to_str(self);
    u32 n = (u32)strlen(s);
    char *o = jalloc(n + 1);
    for(u32 i = 0; i <= n; i++) o[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    return js_str(o);
}

static js_val_t str_lower(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    const char *s = js_to_str(self);
    u32 n = (u32)strlen(s);
    char *o = jalloc(n + 1);
    for(u32 i = 0; i <= n; i++) o[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    return js_str(o);
}

static js_val_t str_index_of(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self), *f = js_to_str(arg(argc, argv, 0));
    u32 n = (u32)strlen(s), m = (u32)strlen(f);
    for(u32 i = 0; i + m <= n; i++) if(memcmp(s + i, f, m) == 0) return js_num(i);
    return js_num(-1);
}

static js_val_t str_includes(js_val_t self, int argc, js_val_t *argv, void *ctx){
    js_val_t r = str_index_of(self, argc, argv, ctx);
    return js_bool(r.n >= 0);
}

static js_val_t str_slice(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    int len = (int)strlen(s);
    int a = argc > 0 ? (int)js_to_num(argv[0]) : 0;
    int b = argc > 1 ? (int)js_to_num(argv[1]) : len;
    if(a < 0) a += len;
    if(b < 0) b += len;
    if(a < 0) a = 0;
    if(b > len) b = len;
    if(b <= a) return js_str("");
    char *o = jalloc((u32)(b - a) + 1);
    memcpy(o, s + a, (u32)(b - a));
    o[b - a] = 0;
    return js_str(o);
}

static js_val_t str_trim(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    const char *s = js_to_str(self);
    while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    u32 n = (u32)strlen(s);
    while(n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r')) n--;
    char *o = jalloc(n + 1);
    memcpy(o, s, n);
    o[n] = 0;
    return js_str(o);
}

static js_val_t str_char_at(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    int i = argc ? (int)js_to_num(argv[0]) : 0;
    char *o = jalloc(2);
    o[0] = (i >= 0 && i < (int)strlen(s)) ? s[i] : 0;
    o[1] = 0;
    return js_str(o);
}

static js_val_t str_repeat(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    int k = argc ? (int)js_to_num(argv[0]) : 0;
    u32 n = (u32)strlen(s);
    if(k <= 0 || n * (u32)k > 16384){ return js_str(""); }
    char *o = jalloc(n * (u32)k + 1);
    for(int i = 0; i < k; i++) memcpy(o + (u32)i * n, s, n);
    o[n * (u32)k] = 0;
    return js_str(o);
}

/* Replaces the first (or every, with g) match of pattern in s. */
static js_val_t replace_impl(js_val_t self, js_val_t pat, js_val_t rep, int all){
    const char *s = js_to_str(self);
    u32 n = (u32)strlen(s);
    const char *r = js_to_str(rep);
    u32 rn = (u32)strlen(r);
    char *out = jalloc(n * 2 + rn * 4 + 16);
    u32 o = 0, pos = 0;
    int isre = pat.t == JS_OBJ && pat.o->is_regex;
    const char *p = isre ? pat.o->re->pat : js_to_str(pat);
    u32 pl = isre ? pat.o->re->plen : (u32)strlen(p);
    int icase = isre && pat.o->re->icase;
    int g = isre ? (all || pat.o->re->global) : all;
    while(pos <= n){
        u32 ms, me;
        int found;
        if(isre) found = re_search(p, pl, icase, s, n, pos, &ms, &me);
        else {
            found = 0;
            for(u32 i = pos; i + pl <= n; i++) if(memcmp(s + i, p, pl) == 0){ ms = i; me = i + pl; found = 1; break; }
        }
        if(!found) break;
        while(o < ms) out[o++] = s[pos++];
        for(u32 i = 0; i < rn; i++) out[o++] = r[i];
        pos = me;
        if(me == ms){ if(pos < n) out[o++] = s[pos]; pos++; }
        if(!g) break;
    }
    while(pos < n) out[o++] = s[pos++];
    out[o] = 0;
    return js_str(out);
}

static js_val_t str_replace(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    return replace_impl(self, arg(argc, argv, 0), arg(argc, argv, 1), 0);
}

static js_val_t str_replace_all(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    return replace_impl(self, arg(argc, argv, 0), arg(argc, argv, 1), 1);
}

static js_val_t str_match(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    js_val_t pat = arg(argc, argv, 0);
    const char *s = js_to_str(self);
    u32 n = (u32)strlen(s);
    int isre = pat.t == JS_OBJ && pat.o->is_regex;
    if(!isre) return js_null();
    js_obj_t *out = new_array();
    u32 pos = 0;
    while(pos <= n){
        u32 ms, me;
        if(!re_search(pat.o->re->pat, pat.o->re->plen, pat.o->re->icase, s, n, pos, &ms, &me)) break;
        arr_push(out, js_str(jstrdup(s + ms, me - ms)));
        if(!pat.o->re->global) break;
        pos = me == ms ? me + 1 : me;
    }
    if(out->nitems == 0) return js_null();
    return js_objv(out);
}

static js_val_t str_split(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    u32 n = (u32)strlen(s);
    js_obj_t *out = new_array();
    js_val_t sep = arg(argc, argv, 0);
    if(sep.t == JS_UNDEF){ arr_push(out, js_str(s)); return js_objv(out); }
    int isre = sep.t == JS_OBJ && sep.o->is_regex;
    const char *p = isre ? sep.o->re->pat : js_to_str(sep);
    u32 pl = isre ? sep.o->re->plen : (u32)strlen(p);
    u32 pos = 0, start = 0;
    while(pos <= n){
        u32 ms = 0, me = 0;
        int found;
        if(pl == 0 && !isre){
            /* split into characters */
            if(pos >= n) break;
            arr_push(out, js_str(jstrdup(s + pos, 1)));
            pos++;
            continue;
        }
        if(isre) found = re_search(p, pl, sep.o->re->icase, s, n, pos, &ms, &me);
        else {
            found = 0;
            for(u32 i = pos; i + pl <= n; i++) if(memcmp(s + i, p, pl) == 0){ ms = i; me = i + pl; found = 1; break; }
        }
        if(!found) break;
        arr_push(out, js_str(jstrdup(s + start, ms - start)));
        start = me;
        pos = me > ms ? me : me + 1;
    }
    arr_push(out, js_str(jstrdup(s + start, n - start)));
    return js_objv(out);
}

static js_val_t regex_test(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_regex) return js_bool(0);
    const char *s = js_to_str(arg(argc, argv, 0));
    u32 ms, me;
    return js_bool(re_search(self.o->re->pat, self.o->re->plen, self.o->re->icase, s, (u32)strlen(s), 0, &ms, &me));
}

/* JSON.stringify: objects, arrays, strings, numbers, booleans and null. */
static void json_put(char *out, u32 *n, u32 cap, const char *s){
    for(; *s && *n + 1 < cap; s++) out[(*n)++] = *s;
}
static void json_value(js_val_t v, char *out, u32 *n, u32 cap){
    if(*n + 8 >= cap) return;
    switch(v.t){
        case JS_NULL: case JS_UNDEF: json_put(out, n, cap, "null"); return;
        case JS_BOOL: json_put(out, n, cap, v.b ? "true" : "false"); return;
        case JS_NUM: json_put(out, n, cap, v.n == v.n ? js_to_str(v) : "null"); return;
        case JS_STR:
            out[(*n)++] = '"';
            for(const char *p = v.s; *p && *n + 3 < cap; p++){
                if(*p == '"' || *p == '\\'){ out[(*n)++] = '\\'; out[(*n)++] = *p; }
                else if(*p == '\n'){ out[(*n)++] = '\\'; out[(*n)++] = 'n'; }
                else out[(*n)++] = *p;
            }
            out[(*n)++] = '"';
            return;
        case JS_OBJ:
            if(v.o->is_func) { json_put(out, n, cap, "null"); return; }
            if(v.o->is_array){
                out[(*n)++] = '[';
                for(int i = 0; i < v.o->nitems; i++){
                    if(i) out[(*n)++] = ',';
                    json_value(v.o->items[i], out, n, cap);
                }
                out[(*n)++] = ']';
                return;
            }
            out[(*n)++] = '{';
            js_obj_t *ks = keys_of(v);
            int first = 1;
            for(int i = 0; i < ks->nitems; i++){
                js_val_t pv = js_get(v.o, ks->items[i].s);
                if(pv.t == JS_UNDEF || (pv.t == JS_OBJ && pv.o->is_func)) continue;
                if(!first) out[(*n)++] = ',';
                first = 0;
                json_value(ks->items[i], out, n, cap);
                out[(*n)++] = ':';
                json_value(pv, out, n, cap);
            }
            out[(*n)++] = '}';
            return;
        default: json_put(out, n, cap, "null"); return;
    }
}
static js_val_t json_stringify(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    char *out = jalloc(8192);
    u32 n = 0;
    js_val_t v = arg(argc, argv, 0);
    if(v.t == JS_UNDEF) return js_undef();
    json_value(v, out, &n, 8191);
    out[n] = 0;
    return js_str(out);
}

/* JSON.parse: objects, arrays, strings, numbers, true, false and null. */
static const char *jp;
static js_val_t json_parse_value(void);
static void json_skip(void){ while(*jp == ' ' || *jp == '\n' || *jp == '\t' || *jp == '\r') jp++; }
static js_val_t json_parse_value(void){
    json_skip();
    if(*jp == '{'){
        jp++;
        js_obj_t *o = js_new_object();
        json_skip();
        while(*jp && *jp != '}'){
            json_skip();
            if(*jp != '"'){ fail("bad JSON key"); return js_undef(); }
            jp++;
            char key[64]; u32 k = 0;
            while(*jp && *jp != '"' && k < 63) key[k++] = *jp++;
            key[k] = 0;
            if(*jp == '"') jp++;
            json_skip();
            if(*jp == ':') jp++;
            js_val_t v = json_parse_value();
            js_set(o, jstrdup(key, k), v);
            json_skip();
            if(*jp == ',') jp++;
            json_skip();
        }
        if(*jp == '}') jp++;
        return js_objv(o);
    }
    if(*jp == '['){
        jp++;
        js_obj_t *a = new_array();
        json_skip();
        while(*jp && *jp != ']'){
            arr_push(a, json_parse_value());
            json_skip();
            if(*jp == ',') jp++;
            json_skip();
        }
        if(*jp == ']') jp++;
        return js_objv(a);
    }
    if(*jp == '"'){
        jp++;
        char *buf = jalloc(256); u32 k = 0;
        while(*jp && *jp != '"'){
            if(*jp == '\\' && jp[1]){ jp++; buf[k++] = *jp == 'n' ? '\n' : *jp; jp++; continue; }
            if(k < 255) buf[k++] = *jp;
            jp++;
        }
        buf[k] = 0;
        if(*jp == '"') jp++;
        return js_str(buf);
    }
    if(strncmp(jp, "true", 4) == 0){ jp += 4; return js_bool(1); }
    if(strncmp(jp, "false", 5) == 0){ jp += 5; return js_bool(0); }
    if(strncmp(jp, "null", 4) == 0){ jp += 4; return js_null(); }
    double d = 0; const char *end = NULL;
    if(kstrtod(jp, &d, &end) && end && end != jp){ jp = end; return js_num(d); }
    fail("bad JSON");
    return js_undef();
}
static js_val_t json_parse(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    jp = js_to_str(arg(argc, argv, 0));
    return json_parse_value();
}
static js_val_t array_is_array(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    js_val_t v = arg(argc, argv, 0);
    return js_bool(v.t == JS_OBJ && v.o->is_array);
}
static js_val_t date_now(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)argc; (void)argv; (void)ctx;
    return js_num((double)timer_ticks() * 10.0);
}

static js_val_t str_char_code_at(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    int i = argc ? (int)js_to_num(argv[0]) : 0;
    int len = (int)strlen(s);
    if(i < 0 || i >= len) return js_num(nan_value());
    return js_num((double)(unsigned char)s[i]);
}
static js_val_t str_starts_with(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    const char *p = js_to_str(arg(argc, argv, 0));
    u32 pl = (u32)strlen(p);
    int off = argc > 1 ? (int)js_to_num(argv[1]) : 0;
    if(off < 0) off = 0;
    if((u32)off + pl > (u32)strlen(s)) return js_bool(0);
    return js_bool(memcmp(s + off, p, pl) == 0);
}
static js_val_t str_ends_with(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    const char *s = js_to_str(self);
    const char *p = js_to_str(arg(argc, argv, 0));
    u32 sl = (u32)strlen(s), pl = (u32)strlen(p);
    u32 end = argc > 1 ? (u32)(int)js_to_num(argv[1]) : sl;
    if(end > sl) end = sl;
    if(pl > end) return js_bool(0);
    return js_bool(memcmp(s + end - pl, p, pl) == 0);
}
static js_val_t str_pad(js_val_t self, int argc, js_val_t *argv, int start){
    const char *s = js_to_str(self);
    u32 sl = (u32)strlen(s);
    int target = argc ? (int)js_to_num(argv[0]) : 0;
    const char *pad = argc > 1 ? js_to_str(argv[1]) : " ";
    u32 pl = (u32)strlen(pad);
    if(target <= (int)sl || pl == 0 || target > 16384) return js_str(s);
    u32 need = (u32)target - sl, w = 0;
    char *o = jalloc((u32)target + 1);
    if(start){
        for(u32 i = 0; i < need; i++) o[w++] = pad[i % pl];
        memcpy(o + w, s, sl); w += sl;
    } else {
        memcpy(o, s, sl); w = sl;
        for(u32 i = 0; i < need; i++) o[w++] = pad[i % pl];
    }
    o[w] = 0;
    return js_str(o);
}
static js_val_t str_pad_start(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return str_pad(self, argc, argv, 1); }
static js_val_t str_pad_end(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return str_pad(self, argc, argv, 0); }
static js_val_t str_from_char_code(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    char *o = jalloc((u32)argc + 1);
    for(int i = 0; i < argc; i++) o[i] = (char)(int)js_to_num(argv[i]);
    o[argc] = 0;
    return js_str(o);
}

static js_val_t arr_reverse(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return self;
    int n = self.o->nitems;
    for(int i = 0; i < n / 2; i++){
        js_val_t t = self.o->items[i];
        self.o->items[i] = self.o->items[n - 1 - i];
        self.o->items[n - 1 - i] = t;
    }
    return self;
}
static js_val_t arr_concat(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    js_obj_t *out = new_array();
    if(self.t == JS_OBJ && self.o->is_array) for(int i = 0; i < self.o->nitems; i++) arr_push(out, self.o->items[i]);
    for(int a = 0; a < argc; a++){
        js_val_t v = argv[a];
        if(v.t == JS_OBJ && v.o->is_array) for(int i = 0; i < v.o->nitems; i++) arr_push(out, v.o->items[i]);
        else arr_push(out, v);
    }
    return js_objv(out);
}
static js_val_t arr_shift(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)argc; (void)argv; (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || self.o->nitems == 0) return js_undef();
    js_val_t r = self.o->items[0];
    for(int i = 1; i < self.o->nitems; i++) self.o->items[i - 1] = self.o->items[i];
    self.o->nitems--;
    return r;
}
static js_val_t arr_unshift(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return js_num(0);
    for(int i = 0; i < argc; i++) arr_push(self.o, js_undef());
    int n = self.o->nitems;
    for(int i = n - 1; i >= argc; i--) self.o->items[i] = self.o->items[i - argc];
    for(int i = 0; i < argc; i++) self.o->items[i] = argv[i];
    return js_num(self.o->nitems);
}
static js_val_t arr_find(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_undef();
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(js_truthy(call_value(argv[0], js_undef(), 3, a))) return self.o->items[i];
    }
    return js_undef();
}
static js_val_t arr_find_index(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_num(-1);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(js_truthy(call_value(argv[0], js_undef(), 3, a))) return js_num(i);
    }
    return js_num(-1);
}
static js_val_t arr_some(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_bool(0);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(js_truthy(call_value(argv[0], js_undef(), 3, a))) return js_bool(1);
    }
    return js_bool(0);
}
static js_val_t arr_every(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_bool(1);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(!js_truthy(call_value(argv[0], js_undef(), 3, a))) return js_bool(0);
    }
    return js_bool(1);
}
static js_val_t arr_sort(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return self;
    js_val_t cmp = argc ? argv[0] : js_undef();
    int has = js_callable(cmp);
    int n = self.o->nitems;
    for(int i = 1; i < n && !STOP(); i++){           /* stable insertion sort */
        js_val_t key = self.o->items[i];
        int j = i - 1;
        while(j >= 0){
            int gt;
            if(has){
                js_val_t a[2] = { self.o->items[j], key };
                gt = js_to_num(call_value(cmp, js_undef(), 2, a)) > 0;
            } else {
                gt = strcmp(js_to_str(self.o->items[j]), js_to_str(key)) > 0;
            }
            if(!gt) break;
            self.o->items[j + 1] = self.o->items[j];
            j--;
        }
        self.o->items[j + 1] = key;
    }
    return self;
}
static js_val_t num_to_string(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    int radix = argc ? (int)js_to_num(argv[0]) : 10;
    if(radix == 10 || radix < 2 || radix > 36) return js_str(js_to_str(self));
    double v = js_to_num(self);
    long long iv = (long long)v;
    int neg = iv < 0;
    unsigned long long u = neg ? (unsigned long long)(-iv) : (unsigned long long)iv;
    const char *digs = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[72]; int k = 0;
    if(u == 0) tmp[k++] = '0';
    while(u){ tmp[k++] = digs[u % (unsigned)radix]; u /= (unsigned)radix; }
    char out[74]; int o = 0;
    if(neg) out[o++] = '-';
    while(k) out[o++] = tmp[--k];
    out[o] = 0;
    return js_str(jstrdup(out, (u32)o));
}

static js_val_t num_to_fixed(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    double v = js_to_num(self);
    int d = argc ? (int)js_to_num(argv[0]) : 0;
    if(d < 0) d = 0;
    if(d > 10) d = 10;
    double scale = 1; for(int i = 0; i < d; i++) scale *= 10;
    double r = v * scale;
    r = r < 0 ? -kfloor(-r + 0.5) : kfloor(r + 0.5);
    char b[48]; int k = 0;
    long long whole = (long long)(r / scale);
    long long frac = (long long)(r - (double)whole * scale);
    if(frac < 0) frac = -frac;
    char t[24]; int tk = 0;
    if(whole == 0 && r < 0) b[k++] = '-';
    if(whole < 0 && whole != 0) { b[k++] = '-'; whole = -whole; }
    do { t[tk++] = (char)('0' + whole % 10); whole /= 10; } while(whole);
    while(tk) b[k++] = t[--tk];
    if(d > 0){
        b[k++] = '.';
        for(int i = d - 1; i >= 0; i--){ int dig = (int)(frac); (void)dig; }
        long long f = frac;
        char fd[12];
        for(int i = d - 1; i >= 0; i--){ fd[i] = (char)('0' + f % 10); f /= 10; }
        for(int i = 0; i < d; i++) b[k++] = fd[i];
    }
    b[k] = 0;
    return js_str(jstrdup(b, (u32)k));
}

static js_val_t g_parse_int(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    const char *s = js_to_str(arg(argc, argv, 0));
    while(*s == ' ') s++;
    int neg = 0;
    if(*s == '-'){ neg = 1; s++; } else if(*s == '+') s++;
    double v = 0; int any = 0;
    while(*s >= '0' && *s <= '9'){ v = v * 10 + (*s - '0'); s++; any = 1; }
    if(!any) return js_num(nan_value());
    return js_num(neg ? -v : v);
}

static js_val_t g_parse_float(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    const char *s = js_to_str(arg(argc, argv, 0));
    double d = 0; const char *end = NULL;
    if(!kstrtod(s, &d, &end)) return js_num(nan_value());
    return js_num(d);
}

static js_val_t g_number(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    return js_num(argc ? js_to_num(argv[0]) : 0);
}
static js_val_t g_string(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    return js_str(argc ? js_to_str(argv[0]) : "");
}
static js_val_t g_boolean(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    return js_bool(argc ? js_truthy(argv[0]) : 0);
}
static js_val_t g_is_nan(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    double v = js_to_num(arg(argc, argv, 0));
    return js_bool(v != v);
}

static js_val_t obj_keys(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    return js_objv(keys_of(arg(argc, argv, 0)));
}

static js_val_t obj_values(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    js_val_t o = arg(argc, argv, 0);
    js_obj_t *out = new_array();
    js_obj_t *ks = keys_of(o);
    for(int i = 0; i < ks->nitems; i++) arr_push(out, js_get(o.o, ks->items[i].s));
    return js_objv(out);
}

static js_val_t math_fn(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self;
    int which = (int)(long)ctx;
    double x = js_to_num(arg(argc, argv, 0));
    switch(which){
        case 0: return js_num(kfloor(x));
        case 1: return js_num(kceil(x));
        case 2: return js_num(kfabs(x));
        case 3: return js_num(ksqrt(x));
        case 4: return js_num(x < 0 ? -kfloor(-x + 0.5) : kfloor(x + 0.5));
        case 5: { double y = js_to_num(arg(argc, argv, 1)); return js_num(kpow(x, y)); }
        case 6: {
            double r = x;
            for(int i = 1; i < argc; i++){ double v = js_to_num(argv[i]); if(v > r) r = v; }
            return js_num(r);
        }
        case 7: {
            double r = x;
            for(int i = 1; i < argc; i++){ double v = js_to_num(argv[i]); if(v < r) r = v; }
            return js_num(r);
        }
        case 8: {
            static unsigned long long seed = 88172645463325252ull;
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            return js_num((double)(seed % 1000000) / 1000000.0);
        }
    }
    return js_undef();
}

/* ---- timers ---- */
typedef struct { int used, id, interval; u64 due; u32 ms; js_val_t fn; } timer_t_;
static timer_t_ timers[32];
static int next_timer_id = 1;

static js_val_t add_timer(js_val_t self, int argc, js_val_t *argv, int repeat){
    (void)self;
    js_val_t fn = arg(argc, argv, 0);
    if(!js_callable(fn)) return js_undef();
    double ms = argc > 1 ? js_to_num(argv[1]) : 0;
    if(ms != ms || ms < 0) ms = 0;
    for(int i = 0; i < 32; i++){
        if(timers[i].used) continue;
        timers[i].used = 1;
        timers[i].id = next_timer_id++;
        timers[i].interval = repeat;
        timers[i].ms = (u32)ms;
        timers[i].due = timer_ticks() + (u64)(ms / 10);
        timers[i].fn = fn;
        return js_num(timers[i].id);
    }
    return js_undef();
}
static js_val_t g_set_timeout(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return add_timer(self, argc, argv, 0); }
static js_val_t g_set_interval(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return add_timer(self, argc, argv, 1); }
static js_val_t g_clear_timer(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    int id = (int)js_to_num(arg(argc, argv, 0));
    for(int i = 0; i < 32; i++) if(timers[i].used && timers[i].id == id) timers[i].used = 0;
    return js_undef();
}

int js_timers_tick(u64 now){
    int ran = 0;
    for(int i = 0; i < 32 && !STOP(); i++){
        if(!timers[i].used || timers[i].due > now) continue;
        js_val_t fn = timers[i].fn;
        if(timers[i].interval) timers[i].due = now + (u64)(timers[i].ms / 10 ? timers[i].ms / 10 : 1);
        else timers[i].used = 0;
        call_value(fn, js_undef(), 0, NULL);
        ran++;
    }
    return ran;
}

/* event objects for handlers: preventDefault() stops the default action */
static js_val_t prevent_default(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)argc; (void)argv; (void)ctx;
    prevented = 1;
    return js_undef();
}

js_val_t js_compile_function(const char *src, u32 len, const char *param){
    if(!arena) js_init();
    lex(src, len);
    ti = 0;
    jsnode_t *f = node(N_FUNC);
    jsnode_t *p = node(N_IDENT);
    p->str = param;
    f->list = p;
    jsnode_t *body = node(N_BLOCK);
    jsnode_t *last = NULL;
    while(toks[ti].t != T_EOF && !STOP()){
        jsnode_t *s = statement();
        if(last) last->next = s; else body->list = s;
        last = s;
    }
    f->a = body;
    if(STOP()) return js_undef();
    return make_fn(f, global_env);
}

/* Gives the script heap back to the kernel (the next js_init makes a new one). */
void js_release(void){
    if(arena) kfree(arena);
    arena = arena_end = NULL;
    free_list = NULL;
    nroots = 0;
    for(int i = 0; i < 32; i++) timers[i].used = 0;
}

void js_init(void){
    heap_reset();
    failed = 0;
    thrown = 0;
    err_msg[0] = 0;
    last_log[0] = 0;
    ctl = 0;
    depth = 0;
    steps = 0;
    prevented = 0;
    for(int i = 0; i < 32; i++) timers[i].used = 0;

    object_proto = new_obj();
    array_proto = new_obj();
    string_proto = new_obj();
    regex_proto = new_obj();
    number_proto = new_obj();
    js_set(number_proto, "toFixed", js_objv(js_new_native(num_to_fixed, NULL)));

    global_obj = js_new_object();
    global_env = new_env(NULL);
    env_define(global_env, "this", js_objv(global_obj));   /* global this === window */

    js_set(array_proto, "push", js_objv(js_new_native(arr_push_native, NULL)));
    js_set(array_proto, "pop", js_objv(js_new_native(arr_pop, NULL)));
    js_set(array_proto, "join", js_objv(js_new_native(arr_join, NULL)));
    js_set(array_proto, "indexOf", js_objv(js_new_native(arr_index_of, NULL)));
    js_set(array_proto, "includes", js_objv(js_new_native(arr_includes, NULL)));
    js_set(array_proto, "slice", js_objv(js_new_native(arr_slice, NULL)));
    js_set(array_proto, "forEach", js_objv(js_new_native(arr_for_each, NULL)));
    js_set(array_proto, "map", js_objv(js_new_native(arr_map, NULL)));
    js_set(array_proto, "filter", js_objv(js_new_native(arr_filter, NULL)));
    js_set(array_proto, "reduce", js_objv(js_new_native(arr_reduce, NULL)));
    js_set(array_proto, "reverse", js_objv(js_new_native(arr_reverse, NULL)));
    js_set(array_proto, "sort", js_objv(js_new_native(arr_sort, NULL)));
    js_set(array_proto, "concat", js_objv(js_new_native(arr_concat, NULL)));
    js_set(array_proto, "shift", js_objv(js_new_native(arr_shift, NULL)));
    js_set(array_proto, "unshift", js_objv(js_new_native(arr_unshift, NULL)));
    js_set(array_proto, "find", js_objv(js_new_native(arr_find, NULL)));
    js_set(array_proto, "findIndex", js_objv(js_new_native(arr_find_index, NULL)));
    js_set(array_proto, "some", js_objv(js_new_native(arr_some, NULL)));
    js_set(array_proto, "every", js_objv(js_new_native(arr_every, NULL)));

    js_set(string_proto, "toUpperCase", js_objv(js_new_native(str_upper, NULL)));
    js_set(string_proto, "toLowerCase", js_objv(js_new_native(str_lower, NULL)));
    js_set(string_proto, "indexOf", js_objv(js_new_native(str_index_of, NULL)));
    js_set(string_proto, "includes", js_objv(js_new_native(str_includes, NULL)));
    js_set(string_proto, "slice", js_objv(js_new_native(str_slice, NULL)));
    js_set(string_proto, "substring", js_objv(js_new_native(str_slice, NULL)));
    js_set(string_proto, "trim", js_objv(js_new_native(str_trim, NULL)));
    js_set(string_proto, "charAt", js_objv(js_new_native(str_char_at, NULL)));
    js_set(string_proto, "repeat", js_objv(js_new_native(str_repeat, NULL)));
    js_set(string_proto, "replace", js_objv(js_new_native(str_replace, NULL)));
    js_set(string_proto, "replaceAll", js_objv(js_new_native(str_replace_all, NULL)));
    js_set(string_proto, "match", js_objv(js_new_native(str_match, NULL)));
    js_set(string_proto, "split", js_objv(js_new_native(str_split, NULL)));
    js_set(string_proto, "charCodeAt", js_objv(js_new_native(str_char_code_at, NULL)));
    js_set(string_proto, "startsWith", js_objv(js_new_native(str_starts_with, NULL)));
    js_set(string_proto, "endsWith", js_objv(js_new_native(str_ends_with, NULL)));
    js_set(string_proto, "padStart", js_objv(js_new_native(str_pad_start, NULL)));
    js_set(string_proto, "padEnd", js_objv(js_new_native(str_pad_end, NULL)));

    js_set(number_proto, "toString", js_objv(js_new_native(num_to_string, NULL)));

    js_set(regex_proto, "test", js_objv(js_new_native(regex_test, NULL)));

    js_obj_t *console = js_new_object();
    js_set(console, "log", js_objv(js_new_native(native_log, NULL)));
    js_set(global_obj, "console", js_objv(console));

    js_obj_t *math = js_new_object();
    js_set(math, "floor", js_objv(js_new_native(math_fn, (void *)0)));
    js_set(math, "ceil", js_objv(js_new_native(math_fn, (void *)1)));
    js_set(math, "abs", js_objv(js_new_native(math_fn, (void *)2)));
    js_set(math, "sqrt", js_objv(js_new_native(math_fn, (void *)3)));
    js_set(math, "round", js_objv(js_new_native(math_fn, (void *)4)));
    js_set(math, "pow", js_objv(js_new_native(math_fn, (void *)5)));
    js_set(math, "max", js_objv(js_new_native(math_fn, (void *)6)));
    js_set(math, "min", js_objv(js_new_native(math_fn, (void *)7)));
    js_set(math, "random", js_objv(js_new_native(math_fn, (void *)8)));
    js_set(math, "PI", js_num(3.141592653589793));
    js_set(global_obj, "Math", js_objv(math));

    js_obj_t *object = js_new_object();
    js_set(object, "keys", js_objv(js_new_native(obj_keys, NULL)));
    js_set(object, "values", js_objv(js_new_native(obj_values, NULL)));
    js_set(global_obj, "Object", js_objv(object));

    js_set(global_obj, "parseInt", js_objv(js_new_native(g_parse_int, NULL)));
    js_set(global_obj, "parseFloat", js_objv(js_new_native(g_parse_float, NULL)));
    js_set(global_obj, "Number", js_objv(js_new_native(g_number, NULL)));
    js_obj_t *string_ctor = js_new_native(g_string, NULL);
    js_set(string_ctor, "fromCharCode", js_objv(js_new_native(str_from_char_code, NULL)));
    js_set(global_obj, "String", js_objv(string_ctor));
    js_set(global_obj, "Boolean", js_objv(js_new_native(g_boolean, NULL)));
    js_set(global_obj, "isNaN", js_objv(js_new_native(g_is_nan, NULL)));
    js_set(global_obj, "setTimeout", js_objv(js_new_native(g_set_timeout, NULL)));
    js_set(global_obj, "setInterval", js_objv(js_new_native(g_set_interval, NULL)));
    js_set(global_obj, "clearTimeout", js_objv(js_new_native(g_clear_timer, NULL)));
    js_set(global_obj, "clearInterval", js_objv(js_new_native(g_clear_timer, NULL)));
    js_set(global_obj, "NaN", js_num(nan_value()));
    js_set(global_obj, "Infinity", js_num(1.0 / 0.0));
    js_obj_t *json = js_new_object();
    js_set(json, "stringify", js_objv(js_new_native(json_stringify, NULL)));
    js_set(json, "parse", js_objv(js_new_native(json_parse, NULL)));
    js_set(global_obj, "JSON", js_objv(json));
    js_set(object, "isArray", js_objv(js_new_native(array_is_array, NULL)));
    js_obj_t *array_ctor = js_new_object();
    js_set(array_ctor, "isArray", js_objv(js_new_native(array_is_array, NULL)));
    js_set(global_obj, "Array", js_objv(array_ctor));
    js_obj_t *date = js_new_object();
    js_set(date, "now", js_objv(js_new_native(date_now, NULL)));
    js_set(global_obj, "Date", js_objv(date));
    /* the globals that hold pointers into the heap */
    js_gc_root(&global_obj, sizeof global_obj);
    js_gc_root(&toks, sizeof toks);
    js_gc_root(&global_env, sizeof global_env);
    js_gc_root(&object_proto, sizeof object_proto);
    js_gc_root(&array_proto, sizeof array_proto);
    js_gc_root(&string_proto, sizeof string_proto);
    js_gc_root(&regex_proto, sizeof regex_proto);
    js_gc_root(&number_proto, sizeof number_proto);
    js_gc_root(&ret_val, sizeof ret_val);
    js_gc_root(&thrown_val, sizeof thrown_val);
    js_gc_root(&toks, sizeof toks);
    js_gc_root(timers, sizeof timers);
}

/* Used by the browser to add the preventDefault function to event objects. */
js_val_t js_prevent_default_fn(void){
    return js_objv(js_new_native(prevent_default, NULL));
}

int js_run(const char *src, u32 len){
    if(!arena) js_init();
    failed = 0;
    thrown = 0;
    err_msg[0] = 0;
    ctl = 0;
    steps = 0;
    lex(src, len);
    if(failed) return -1;
    ti = 0;
    jsnode_t *prog = node(N_BLOCK);
    jsnode_t *last = NULL;
    char parse_err[160] = "";
    while(toks[ti].t != T_EOF && !thrown){
        int start = ti;
        failed = 0;
        jsnode_t *s = statement();
        if(failed){
            /* a statement we cannot parse: remember why, skip it, and carry on */
            if(!parse_err[0]) ui_copy_js(parse_err, err_msg, sizeof parse_err);
            failed = 0;
            int depth = 0;
            while(toks[ti].t != T_EOF){
                if(tis("{")) depth++;
                else if(tis("}")){ depth--; if(depth <= 0){ adv(); break; } }
                else if(tis(";") && depth <= 0){ adv(); break; }
                adv();
            }
            if(ti == start) adv();
            continue;
        }
        if(last) last->next = s; else prog->list = s;
        last = s;
    }
    if(thrown){
        if(thrown){
            char m[120] = "Uncaught ";
            const char *t = js_to_str(thrown_val);
            u32 k = 9;
            for(const char *q = t; *q && k < 110; q++) m[k++] = *q;
            m[k] = 0;
            failed = 1; thrown = 0;
            u32 i = 0;
            while(m[i] && i + 1 < sizeof err_msg){ err_msg[i] = m[i]; i++; }
            err_msg[i] = 0;
        }
        return -1;
    }
    for(jsnode_t *s = prog->list; s && !STOP(); s = s->next)
        if(s->k == N_FUNCDECL) env_define(global_env, s->str, make_fn(s->a, global_env));
    for(jsnode_t *s = prog->list; s && !STOP(); s = s->next){
        if(s->k == N_FUNCDECL) continue;
        exec(s, global_env);
        ctl = 0;
    }
    if(thrown){
        char m[120] = "Uncaught ";
        const char *t = js_to_str(thrown_val);
        u32 k = 9;
        for(const char *q = t; *q && k < 110; q++) m[k++] = *q;
        m[k] = 0;
        thrown = 0;
        failed = 1;
        u32 i = 0;
        while(m[i] && i + 1 < sizeof err_msg){ err_msg[i] = m[i]; i++; }
        err_msg[i] = 0;
    }
    if(!failed && parse_err[0]){
        ui_copy_js(err_msg, parse_err, sizeof err_msg);
        return -1;
    }
    return failed ? -1 : 0;
}
