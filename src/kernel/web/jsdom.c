/* The page's script side: the document object, elements as JavaScript objects,
 * text and attributes, and click handlers. */
#include "amdroid/web.h"
#include "amdroid/js.h"
#include "amdroid/heap.h"
#include "amdroid/timer.h"
#include "web_internal.h"
#include "string.h"
#include "amdroid/js.h"

#define MAX_LISTENERS 64

typedef struct { wnode_t *n; char type[32]; js_val_t fn; } listener_t;
static listener_t listeners[MAX_LISTENERS];
static int nlisteners = 0;

static int dirty = 0;
static char js_err[160] = "";
static js_obj_t *document = NULL;

void dom_mark_dirty(void){ dirty = 1; }
int web_take_prevented(void){ return js_take_prevented(); }
int web_tick(void){
    int ran = js_timers_tick(timer_ticks());
    if(ran) dirty = 1;
    return ran;
}
int web_dirty(void){ return dirty; }
void web_clear_dirty(void){ dirty = 0; }
const char *web_js_error(void){ return js_err; }
const char *web_console(void){ return js_last_log(); }

void dom_reset(void){
    nlisteners = 0;
    document = NULL;
    dirty = 0;
    js_err[0] = 0;
}

static js_obj_t *wrap(wnode_t *n);

/* The address of the page, for window.location (the browser sets it before loading). */
static char page_url[1024] = "about:blank";
void web_set_url(const char *url){
    u32 k = 0;
    while(url[k] && k + 1 < sizeof page_url){ page_url[k] = url[k]; k++; }
    page_url[k] = 0;
}

/* Methods every element shares (made once per page; kept alive by the collector root). */
enum { M_QS, M_QSA, M_BYTAG, M_BYCLASS, M_REMOVE_CHILD, M_REMOVE, M_NOOP, M_INSERT_BEFORE, M_COUNT };
static js_obj_t *methods[M_COUNT];

/* The text under an element, in order. */
static void text_of(wnode_t *n, char *out, u32 cap, u32 *len){
    if(n->text){
        for(u32 i = 0; i < n->tlen && *len + 1 < cap; i++) out[(*len)++] = n->txt[i];
        out[*len] = 0;
        return;
    }
    for(wnode_t *c = n->first; c; c = c->next) text_of(c, out, cap, len);
}

static wnode_t *find_id(wnode_t *n, const char *id){
    if(!n->text && n->id && strcmp(n->id, id) == 0) return n;
    for(wnode_t *c = n->first; c; c = c->next){
        wnode_t *r = find_id(c, id);
        if(r) return r;
    }
    return NULL;
}

static wnode_t *find_tag(wnode_t *n, const char *tag){
    if(!n->text && strcmp(n->tag, tag) == 0) return n;
    for(wnode_t *c = n->first; c; c = c->next){
        wnode_t *r = find_tag(c, tag);
        if(r) return r;
    }
    return NULL;
}

static void clear_children(wnode_t *n){
    n->first = n->last = NULL;
}

static void add_text_child(wnode_t *n, const char *s){
    wnode_t *t = web_new_node();
    if(!t) return;
    u32 len = (u32)strlen(s);
    t->text = 1;
    t->txt = kmalloc(len + 1);
    if(!t->txt) return;
    memcpy(t->txt, s, len + 1);
    t->tlen = len;
    t->parent = n;
    if(!n->first) n->first = t; else n->last->next = t;
    n->last = t;
}

/* ---- element objects ---- */

static int elem_get(js_obj_t *o, const char *name, js_val_t *out);
static int elem_set(js_obj_t *o, const char *name, js_val_t v);

static js_val_t add_listener(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || argc < 2 || !js_callable(argv[1])) return js_undef();
    if(nlisteners >= MAX_LISTENERS) return js_undef();
    wnode_t *n = (wnode_t *)js_host(self.o);
    listener_t *l = &listeners[nlisteners++];
    l->n = n;
    u32 k = 0;
    const char *t = js_to_str(argv[0]);
    while(t[k] && k + 1 < sizeof l->type){ l->type[k] = t[k]; k++; }
    l->type[k] = 0;
    l->fn = argv[1];
    return js_undef();
}

static js_val_t get_by_id(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(!web_root || argc < 1) return js_null();
    wnode_t *n = find_id(web_root, js_to_str(argv[0]));
    return n ? js_objv(wrap(n)) : js_null();
}

static js_val_t create_element(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 1) return js_null();
    wnode_t *n = web_new_node();
    if(!n) return js_null();
    const char *tag = js_to_str(argv[0]);
    u32 k = 0;
    while(tag[k] && k + 1 < sizeof n->tag){ n->tag[k] = tag[k]; k++; }
    n->tag[k] = 0;
    n->st.display = W_INLINE;
    return js_objv(wrap(n));
}

static js_val_t get_attr(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || argc < 1) return js_null();
    wnode_t *n = (wnode_t *)js_host(self.o);
    const char *name = js_to_str(argv[0]);
    const char *v = NULL;
    if(strcmp(name, "id") == 0) v = n->id;
    else if(strcmp(name, "class") == 0) v = n->cls;
    else if(strcmp(name, "href") == 0) v = n->href;
    else if(strcmp(name, "alt") == 0) v = n->alt;
    else if(strcmp(name, "style") == 0) v = n->style;
    return v ? js_str(v) : js_null();
}

static js_val_t set_attr(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || argc < 2) return js_undef();
    wnode_t *n = (wnode_t *)js_host(self.o);
    const char *name = js_to_str(argv[0]);
    const char *v = js_to_str(argv[1]);
    if(strcmp(name, "id") == 0) n->id = (char *)v;
    else if(strcmp(name, "class") == 0){ n->cls = (char *)v; dirty = 1; }
    else if(strcmp(name, "href") == 0) n->href = (char *)v;
    else if(strcmp(name, "alt") == 0) n->alt = (char *)v;
    else if(strcmp(name, "style") == 0){ n->style = (char *)v; dirty = 1; }
    return js_undef();
}

static js_val_t append_child(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(self.t != JS_OBJ || argc < 1 || argv[0].t != JS_OBJ) return js_undef();
    wnode_t *p = (wnode_t *)js_host(self.o);
    wnode_t *c = (wnode_t *)js_host(argv[0].o);
    if(!p || !c) return js_undef();
    c->next = NULL;
    c->parent = p;
    if(!p->first) p->first = c; else p->last->next = c;
    p->last = c;
    dirty = 1;
    return argv[0];
}

/* ---- finding elements ---- */

static void collect(wnode_t *n, int mode, const char *what, js_obj_t *out, int first_only, int *found){
    for(wnode_t *c = n->first; c; c = c->next){
        if(c->text) continue;
        if(first_only && *found) return;
        int hit = mode == 0 ? css_matches(c, what) :
                  mode == 1 ? (strcmp(what, "*") == 0 || strcmp(c->tag, what) == 0) :
                  (c->cls && css_matches(c, what));
        if(hit){ js_array_push(out, js_objv(wrap(c))); (*found)++; if(first_only) return; }
        collect(c, mode, what, out, first_only, found);
    }
}

static wnode_t *self_node(js_val_t self){
    if(self.t == JS_OBJ && self.o != document){ wnode_t *n = (wnode_t *)js_host(self.o); if(n) return n; }
    return web_root;
}

static js_val_t find_impl(js_val_t self, int argc, js_val_t *argv, int mode, int first_only){
    js_obj_t *out = js_new_array();
    wnode_t *root = self_node(self);
    if(argc < 1 || !root) return first_only ? js_null() : js_objv(out);
    char sel[160];
    const char *q = js_to_str(argv[0]);
    u32 k = 0;
    if(mode == 2){
        /* getElementsByClassName("a b") is the selector ".a.b" */
        sel[k++] = '.';
        for(; *q && k + 2 < sizeof sel; q++) sel[k++] = *q == ' ' ? '.' : *q;
    } else {
        for(; *q && k + 1 < sizeof sel; q++) sel[k++] = mode == 1 && *q >= 'A' && *q <= 'Z' ? (char)(*q + 32) : *q;
    }
    sel[k] = 0;
    int found = 0;
    collect(root, mode, sel, out, first_only, &found);
    if(first_only) return found ? js_get(out, "0") : js_null();
    return js_objv(out);
}
static js_val_t m_qs(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return find_impl(self, argc, argv, 0, 1); }
static js_val_t m_qsa(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return find_impl(self, argc, argv, 0, 0); }
static js_val_t m_bytag(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return find_impl(self, argc, argv, 1, 0); }
static js_val_t m_byclass(js_val_t self, int argc, js_val_t *argv, void *ctx){ (void)ctx; return find_impl(self, argc, argv, 2, 0); }

static void unlink_child(wnode_t *p, wnode_t *c){
    wnode_t *prev = NULL;
    for(wnode_t *x = p->first; x; prev = x, x = x->next){
        if(x != c) continue;
        if(prev) prev->next = x->next; else p->first = x->next;
        if(p->last == x) p->last = prev;
        x->next = NULL;
        x->parent = NULL;
        dirty = 1;
        return;
    }
}
static js_val_t m_remove_child(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(argc < 1 || argv[0].t != JS_OBJ) return js_undef();
    wnode_t *p = self_node(self), *c = (wnode_t *)js_host(argv[0].o);
    if(p && c) unlink_child(p, c);
    return argv[0];
}
static js_val_t m_remove(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx; (void)argc; (void)argv;
    wnode_t *c = self.t == JS_OBJ ? (wnode_t *)js_host(self.o) : NULL;
    if(c && c->parent) unlink_child(c->parent, c);
    return js_undef();
}
static js_val_t m_insert_before(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)ctx;
    if(argc < 1 || argv[0].t != JS_OBJ) return js_undef();
    wnode_t *p = self_node(self), *c = (wnode_t *)js_host(argv[0].o);
    wnode_t *ref = argc > 1 && argv[1].t == JS_OBJ ? (wnode_t *)js_host(argv[1].o) : NULL;
    if(!p || !c) return js_undef();
    if(c->parent) unlink_child(c->parent, c);
    c->parent = p;
    if(!ref || ref->parent != p){            /* no reference: append */
        c->next = NULL;
        if(!p->first) p->first = c; else p->last->next = c;
        p->last = c;
    } else {
        wnode_t *prev = NULL;
        for(wnode_t *x = p->first; x && x != ref; x = x->next) prev = x;
        c->next = ref;
        if(prev) prev->next = c; else p->first = c;
    }
    dirty = 1;
    return argv[0];
}
static js_val_t m_noop(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)argc; (void)argv; (void)ctx;
    return js_undef();
}

/* ---- classList and style ---- */

static int class_has(const char *list, const char *c){
    u32 cl = (u32)strlen(c);
    if(!list || !cl) return 0;
    for(const char *p = list; *p; ){
        while(*p == ' ') p++;
        const char *b = p;
        while(*p && *p != ' ') p++;
        if((u32)(p - b) == cl && memcmp(b, c, cl) == 0) return 1;
    }
    return 0;
}
static void class_edit(wnode_t *n, const char *c, int add){
    u32 cl = (u32)strlen(c);
    if(!cl) return;
    u32 old = n->cls ? (u32)strlen(n->cls) : 0;
    char *out = kmalloc(old + cl + 2);
    if(!out) return;
    u32 k = 0;
    if(n->cls){
        for(const char *p = n->cls; *p; ){
            while(*p == ' ') p++;
            const char *b = p;
            while(*p && *p != ' ') p++;
            u32 wl = (u32)(p - b);
            if(!wl || (wl == cl && memcmp(b, c, cl) == 0)) continue;     /* drop it (added again below) */
            if(k) out[k++] = ' ';
            memcpy(out + k, b, wl);
            k += wl;
        }
    }
    if(add){ if(k) out[k++] = ' '; memcpy(out + k, c, cl); k += cl; }
    out[k] = 0;
    n->cls = out;          /* the old string stays in the pool: they are small */
    dirty = 1;
}
static js_val_t cl_add(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; for(int i = 0; i < argc; i++) class_edit((wnode_t *)ctx, js_to_str(argv[i]), 1); return js_undef();
}
static js_val_t cl_remove(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; for(int i = 0; i < argc; i++) class_edit((wnode_t *)ctx, js_to_str(argv[i]), 0); return js_undef();
}
static js_val_t cl_contains(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; wnode_t *n = (wnode_t *)ctx;
    return js_bool(argc > 0 && class_has(n->cls, js_to_str(argv[0])));
}
static js_val_t cl_toggle(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; wnode_t *n = (wnode_t *)ctx;
    if(argc < 1) return js_bool(0);
    const char *c = js_to_str(argv[0]);
    int on = argc > 1 ? js_truthy(argv[1]) : !class_has(n->cls, c);
    class_edit(n, c, on);
    return js_bool(on);
}
static js_obj_t *class_list(wnode_t *n){
    js_obj_t *o = js_new_object();
    js_set(o, "add", js_objv(js_new_native(cl_add, n)));
    js_set(o, "remove", js_objv(js_new_native(cl_remove, n)));
    js_set(o, "contains", js_objv(js_new_native(cl_contains, n)));
    js_set(o, "toggle", js_objv(js_new_native(cl_toggle, n)));
    return o;
}

/* el.style.fooBar = v adds "foo-bar: v" to the element's inline style. */
static int style_set(js_obj_t *o, const char *name, js_val_t v){
    wnode_t *n = (wnode_t *)js_host(o);
    if(!n) return 0;
    char prop[48];
    u32 k = 0;
    for(const char *q = name; *q && k + 2 < sizeof prop; q++){
        if(*q >= 'A' && *q <= 'Z'){ prop[k++] = '-'; prop[k++] = (char)(*q + 32); }
        else prop[k++] = *q;
    }
    prop[k] = 0;
    if(strcmp(prop, "css-text") == 0){ n->style = (char *)js_to_str(v); dirty = 1; return 1; }
    const char *val = js_to_str(v);
    u32 old = n->style ? (u32)strlen(n->style) : 0, pl = k, vl = (u32)strlen(val);
    char *out = kmalloc(old + pl + vl + 4);
    if(!out) return 1;
    u32 j = 0;
    if(old){ memcpy(out, n->style, old); j = old; out[j++] = ';'; }
    memcpy(out + j, prop, pl); j += pl;
    out[j++] = ':';
    memcpy(out + j, val, vl); j += vl;
    out[j] = 0;
    n->style = out;       /* later declarations win, as in CSS */
    dirty = 1;
    return 1;
}
static int style_get(js_obj_t *o, const char *name, js_val_t *out){
    (void)o;
    if(strcmp(name, "setProperty") == 0 || strcmp(name, "removeProperty") == 0){ *out = js_objv(methods[M_NOOP]); return 1; }
    *out = js_str("");
    return 1;
}

static int doc_get(js_obj_t *o, const char *name, js_val_t *out){
    (void)o;
    if(strcmp(name, "getElementById") == 0){ *out = js_objv(js_new_native(get_by_id, NULL)); return 1; }
    if(strcmp(name, "createElement") == 0){ *out = js_objv(js_new_native(create_element, NULL)); return 1; }
    if(strcmp(name, "body") == 0){
        wnode_t *b = web_root ? find_tag(web_root, "body") : NULL;
        *out = b ? js_objv(wrap(b)) : js_null();
        return 1;
    }
    if(strcmp(name, "title") == 0){ *out = js_str(web_title()); return 1; }
    if(strcmp(name, "querySelector") == 0){ *out = js_objv(methods[M_QS]); return 1; }
    if(strcmp(name, "querySelectorAll") == 0){ *out = js_objv(methods[M_QSA]); return 1; }
    if(strcmp(name, "getElementsByTagName") == 0){ *out = js_objv(methods[M_BYTAG]); return 1; }
    if(strcmp(name, "getElementsByClassName") == 0){ *out = js_objv(methods[M_BYCLASS]); return 1; }
    if(strcmp(name, "documentElement") == 0 || strcmp(name, "head") == 0){
        wnode_t *h = web_root ? find_tag(web_root, name[0] == 'h' ? "head" : "html") : NULL;
        *out = h ? js_objv(wrap(h)) : js_null();
        return 1;
    }
    if(strcmp(name, "readyState") == 0){ *out = js_str("complete"); return 1; }
    if(strcmp(name, "cookie") == 0){ *out = js_str(""); return 1; }
    if(strcmp(name, "location") == 0){ *out = js_get(js_global(), "location"); return 1; }
    return 0;
}

static js_obj_t *wrap(wnode_t *n){
    if(n->jsobj) return (js_obj_t *)n->jsobj;
    js_obj_t *o = js_new_object();
    js_set_hooks(o, elem_get, elem_set, n);
    js_set(o, "appendChild", js_objv(js_new_native(append_child, NULL)));
    js_set(o, "addEventListener", js_objv(js_new_native(add_listener, NULL)));
    js_set(o, "getAttribute", js_objv(js_new_native(get_attr, NULL)));
    js_set(o, "setAttribute", js_objv(js_new_native(set_attr, NULL)));
    n->jsobj = o;
    return o;
}

static int elem_get(js_obj_t *o, const char *name, js_val_t *out){
    wnode_t *n = (wnode_t *)js_host(o);
    if(!n) return 0;
    if(strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0){
        static char buf[4096];
        u32 len = 0;
        text_of(n, buf, sizeof buf, &len);
        *out = js_str(buf);
        return 1;
    }
    if(strcmp(name, "id") == 0){ *out = js_str(n->id ? n->id : ""); return 1; }
    if(strcmp(name, "querySelector") == 0){ *out = js_objv(methods[M_QS]); return 1; }
    if(strcmp(name, "querySelectorAll") == 0){ *out = js_objv(methods[M_QSA]); return 1; }
    if(strcmp(name, "getElementsByTagName") == 0){ *out = js_objv(methods[M_BYTAG]); return 1; }
    if(strcmp(name, "getElementsByClassName") == 0){ *out = js_objv(methods[M_BYCLASS]); return 1; }
    if(strcmp(name, "removeChild") == 0){ *out = js_objv(methods[M_REMOVE_CHILD]); return 1; }
    if(strcmp(name, "remove") == 0){ *out = js_objv(methods[M_REMOVE]); return 1; }
    if(strcmp(name, "insertBefore") == 0){ *out = js_objv(methods[M_INSERT_BEFORE]); return 1; }
    if(strcmp(name, "removeEventListener") == 0 || strcmp(name, "focus") == 0 || strcmp(name, "blur") == 0 ||
       strcmp(name, "scrollIntoView") == 0){ *out = js_objv(methods[M_NOOP]); return 1; }
    if(strcmp(name, "classList") == 0){ *out = js_objv(class_list(n)); return 1; }
    if(strcmp(name, "style") == 0){
        js_obj_t *st = js_new_object();
        js_set_hooks(st, style_get, style_set, n);
        *out = js_objv(st);
        return 1;
    }
    if(strcmp(name, "parentNode") == 0 || strcmp(name, "parentElement") == 0){
        *out = n->parent ? js_objv(wrap(n->parent)) : js_null();
        return 1;
    }
    if(strcmp(name, "children") == 0 || strcmp(name, "childNodes") == 0){
        js_obj_t *a = js_new_array();
        for(wnode_t *c = n->first; c; c = c->next) if(!c->text || name[5] == 'N') js_array_push(a, js_objv(wrap(c)));
        *out = js_objv(a);
        return 1;
    }
    if(strcmp(name, "firstElementChild") == 0){
        wnode_t *c = n->first;
        while(c && c->text) c = c->next;
        *out = c ? js_objv(wrap(c)) : js_null();
        return 1;
    }
    if(strcmp(name, "nextElementSibling") == 0){
        wnode_t *c = n->next;
        while(c && c->text) c = c->next;
        *out = c ? js_objv(wrap(c)) : js_null();
        return 1;
    }
    if(strcmp(name, "nodeType") == 0){ *out = js_num(n->text ? 3 : 1); return 1; }
    if(strcmp(name, "innerHTML") == 0){
        static char buf[4096];
        u32 len = 0;
        text_of(n, buf, sizeof buf, &len);
        *out = js_str(buf);
        return 1;
    }
    if(strcmp(name, "href") == 0 && n->href){ *out = js_str(n->href); return 1; }
    if(strcmp(name, "value") == 0){ *out = js_str(n->value ? n->value : ""); return 1; }
    if(strcmp(name, "className") == 0){ *out = js_str(n->cls ? n->cls : ""); return 1; }
    if(strcmp(name, "tagName") == 0){
        static char up[12];
        u32 k = 0;
        for(; n->tag[k] && k + 1 < sizeof up; k++) up[k] = (n->tag[k] >= 'a' && n->tag[k] <= 'z') ? (char)(n->tag[k] - 32) : n->tag[k];
        up[k] = 0;
        *out = js_str(up);
        return 1;
    }
    return 0;
}

static int elem_set(js_obj_t *o, const char *name, js_val_t v){
    wnode_t *n = (wnode_t *)js_host(o);
    if(!n) return 0;
    if(strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0){
        clear_children(n);
        add_text_child(n, js_to_str(v));
        dirty = 1;
        return 1;
    }
    if(strcmp(name, "innerHTML") == 0){
        /* no HTML fragments yet: the text of the markup, without its tags */
        const char *src = js_to_str(v);
        u32 sl = (u32)strlen(src), k = 0;
        char *t = kmalloc(sl + 1);
        if(!t) return 1;
        int in_tag = 0;
        for(u32 i = 0; i < sl; i++){
            if(src[i] == '<') in_tag = 1;
            else if(src[i] == '>' && in_tag) in_tag = 0;
            else if(!in_tag) t[k++] = src[i];
        }
        t[k] = 0;
        clear_children(n);
        if(k) add_text_child(n, t);
        kfree(t);
        dirty = 1;
        return 1;
    }
    if(strcmp(name, "style") == 0){ n->style = (char *)js_to_str(v); dirty = 1; return 1; }
    if(strcmp(name, "id") == 0){ n->id = (char *)js_to_str(v); return 1; }
    if(strcmp(name, "value") == 0){ n->value = (char *)js_to_str(v); dirty = 1; return 1; }
    if(strcmp(name, "className") == 0){ n->cls = (char *)js_to_str(v); dirty = 1; return 1; }
    return 0;
}

/* ---- clicks ---- */

int web_dispatch(const void *node, const char *type, const char *key){
    int ran = 0;
    js_obj_t *ev = js_new_object();
    js_set(ev, "type", js_str(type));
    if(key) js_set(ev, "key", js_str(key));
    js_set(ev, "preventDefault", js_prevent_default_fn());
    js_val_t evv = js_objv(ev);
    js_take_prevented();
    for(wnode_t *n = (wnode_t *)node; n; n = n->parent){
        if(n->text) continue;
        for(int i = 0; i < nlisteners; i++){
            if(listeners[i].n != n || strcmp(listeners[i].type, type) != 0) continue;
            js_val_t r = js_call(listeners[i].fn, js_objv(wrap(n)), 1, &evv);
            if(r.t == JS_BOOL && !r.b) js_mark_prevented();
            ran++;
        }
        if(n->jsobj){
            char prop[24] = "on";
            u32 pk = 2;
            for(const char *q = type; *q && pk < 22; q++) prop[pk++] = *q;
            prop[pk] = 0;
            js_val_t h = js_get((js_obj_t *)n->jsobj, prop);
            if(js_callable(h)){
                js_val_t r = js_call(h, js_objv((js_obj_t *)n->jsobj), 1, &evv);
                if(r.t == JS_BOOL && !r.b) js_mark_prevented();
                ran++;
            }
        }
    }
    return ran;
}

int web_click(const void *node){ return web_dispatch(node, "click", NULL); }

/* A key typed into a text box: edits its value and fires keydown and input. */
int web_is_input(const void *node){
    const wnode_t *n = (const wnode_t *)node;
    return n && !n->text && strcmp(n->tag, "input") == 0;
}

int web_key_input(const void *node, int c){
    wnode_t *n = (wnode_t *)node;
    if(!web_is_input(n)) return 0;
    char key[2] = { 0, 0 };
    u32 len = n->value ? (u32)strlen(n->value) : 0;
    char *v = kmalloc(len + 2);
    if(!v) return 1;
    if(n->value) memcpy(v, n->value, len);
    v[len] = 0;
    if(c == '\b'){
        if(len) v[len - 1] = 0;
    } else if(c >= 32 && c <= 126){
        v[len] = (char)c;
        v[len + 1] = 0;
        key[0] = (char)c;
    } else {
        kfree(v);
        return 0;
    }
    n->value = v;
    dirty = 1;
    web_dispatch(n, "keydown", key[0] ? key : "Backspace");
    web_dispatch(n, "input", NULL);
    return 1;
}

/* ---- running the page's scripts ---- */

static void attach_attributes(wnode_t *n){
    if(n->text) return;
    if(n->onclick){
        u32 len = (u32)strlen(n->onclick);
        js_val_t f = js_compile_function(n->onclick, len, "event");
        if(js_callable(f)) js_set(wrap(n), "onclick", f);
    }
    for(wnode_t *c = n->first; c; c = c->next) attach_attributes(c);
}

/* ---- window ---- */

/* addEventListener on window or document: kept with no element, fired after the scripts
 * (DOMContentLoaded, then load). */
static js_val_t global_listen(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 2 || !js_callable(argv[1]) || nlisteners >= MAX_LISTENERS) return js_undef();
    listener_t *l = &listeners[nlisteners++];
    l->n = NULL;
    u32 k = 0;
    const char *t = js_to_str(argv[0]);
    while(t[k] && k + 1 < sizeof l->type){ l->type[k] = t[k]; k++; }
    l->type[k] = 0;
    l->fn = argv[1];
    return js_undef();
}

static js_val_t st_get(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self;
    if(argc < 1) return js_null();
    js_val_t v = js_get((js_obj_t *)ctx, js_to_str(argv[0]));
    return v.t == JS_STR ? v : js_null();
}
static js_val_t st_set(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self;
    if(argc >= 2) js_set((js_obj_t *)ctx, js_to_str(argv[0]), js_str(js_to_str(argv[1])));
    return js_undef();
}
static js_val_t st_remove(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self;
    if(argc >= 1) js_set((js_obj_t *)ctx, js_to_str(argv[0]), js_undef());
    return js_undef();
}
static js_obj_t *storage(void){
    js_obj_t *data = js_new_object();
    js_obj_t *o = js_new_object();
    js_set(o, "getItem", js_objv(js_new_native(st_get, data)));
    js_set(o, "setItem", js_objv(js_new_native(st_set, data)));
    js_set(o, "removeItem", js_objv(js_new_native(st_remove, data)));
    js_set(o, "clear", js_objv(methods[M_NOOP]));
    return o;
}

static js_val_t match_media(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    js_obj_t *o = js_new_object();
    js_set(o, "matches", js_bool(0));
    js_set(o, "media", argc ? js_str(js_to_str(argv[0])) : js_str(""));
    js_set(o, "addListener", js_objv(methods[M_NOOP]));
    js_set(o, "removeListener", js_objv(methods[M_NOOP]));
    js_set(o, "addEventListener", js_objv(methods[M_NOOP]));
    js_set(o, "removeEventListener", js_objv(methods[M_NOOP]));
    return js_objv(o);
}

static js_val_t raf(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)ctx;
    if(argc < 1) return js_num(0);
    js_val_t a[2] = { argv[0], js_num(16) };
    return js_call(js_get(js_global(), "setTimeout"), js_undef(), 2, a);
}

static js_val_t computed_style(js_val_t self, int argc, js_val_t *argv, void *ctx){
    (void)self; (void)argc; (void)argv; (void)ctx;
    js_obj_t *st = js_new_object();
    js_set_hooks(st, style_get, NULL, NULL);
    return js_objv(st);
}

/* window.location from the page address */
static js_obj_t *make_location(void){
    js_obj_t *l = js_new_object();
    const char *u = page_url;
    js_set(l, "href", js_str(u));
    /* static: the strings are used by the page's scripts until the next page */
    static char proto[12], host[96], path[200], search[120], hash[64], origin[120], hostname[96];
    u32 i = 0, k = 0;
    while(u[i] && u[i] != ':' && k + 2 < sizeof proto) proto[k++] = u[i++];
    if(u[i] == ':') proto[k++] = u[i++];
    proto[k] = 0;
    if(u[i] == '/' && u[i + 1] == '/') i += 2;
    k = 0;
    while(u[i] && u[i] != '/' && u[i] != '?' && u[i] != '#' && k + 1 < sizeof host) host[k++] = u[i++];
    host[k] = 0;
    k = 0;
    while(u[i] && u[i] != '?' && u[i] != '#' && k + 1 < sizeof path) path[k++] = u[i++];
    if(k == 0) path[k++] = '/';
    path[k] = 0;
    k = 0;
    while(u[i] && u[i] != '#' && k + 1 < sizeof search) search[k++] = u[i++];
    search[k] = 0;
    k = 0;
    while(u[i] && k + 1 < sizeof hash) hash[k++] = u[i++];
    hash[k] = 0;
    k = 0;
    for(const char *q = proto; *q && k + 1 < sizeof origin; q++) origin[k++] = *q;
    if(k + 3 < sizeof origin){ origin[k++] = '/'; origin[k++] = '/'; }
    for(const char *q = host; *q && k + 1 < sizeof origin; q++) origin[k++] = *q;
    origin[k] = 0;
    k = 0;
    while(host[k] && host[k] != ':' && k + 1 < sizeof hostname){ hostname[k] = host[k]; k++; }
    hostname[k] = 0;
    js_set(l, "protocol", js_str(proto));
    js_set(l, "host", js_str(host));
    js_set(l, "hostname", js_str(hostname));
    js_set(l, "pathname", js_str(path));
    js_set(l, "search", js_str(search));
    js_set(l, "hash", js_str(hash));
    js_set(l, "origin", js_str(origin));
    js_set(l, "reload", js_objv(methods[M_NOOP]));
    js_set(l, "assign", js_objv(methods[M_NOOP]));
    js_set(l, "replace", js_objv(methods[M_NOOP]));
    return l;
}

static void setup_window(void){
    static const js_native_fn fns[M_COUNT] = { m_qs, m_qsa, m_bytag, m_byclass, m_remove_child, m_remove, m_noop, m_insert_before };
    for(int i = 0; i < M_COUNT; i++) methods[i] = js_new_native(fns[i], NULL);
    js_obj_t *g = js_global();
    js_set(g, "window", js_objv(g));
    js_set(g, "self", js_objv(g));
    js_set(g, "globalThis", js_objv(g));
    js_set(g, "top", js_objv(g));
    js_set(g, "parent", js_objv(g));
    js_set(g, "location", js_objv(make_location()));
    js_obj_t *nav = js_new_object();
    js_set(nav, "userAgent", js_str("Mozilla/5.0 (am-droid) BRO-weser/1.0"));
    js_set(nav, "language", js_str("en-US"));
    js_set(nav, "platform", js_str("am-droid"));
    js_set(nav, "cookieEnabled", js_bool(0));
    js_set(nav, "onLine", js_bool(1));
    js_set(g, "navigator", js_objv(nav));
    js_set(g, "localStorage", js_objv(storage()));
    js_set(g, "sessionStorage", js_objv(storage()));
    js_set(g, "matchMedia", js_objv(js_new_native(match_media, NULL)));
    js_set(g, "requestAnimationFrame", js_objv(js_new_native(raf, NULL)));
    js_set(g, "cancelAnimationFrame", js_get(g, "clearTimeout"));
    js_set(g, "getComputedStyle", js_objv(js_new_native(computed_style, NULL)));
    js_obj_t *listen = js_new_native(global_listen, NULL);
    js_set(g, "addEventListener", js_objv(listen));
    js_set(g, "removeEventListener", js_objv(methods[M_NOOP]));
    js_set(g, "scrollTo", js_objv(methods[M_NOOP]));
    js_set(g, "scrollBy", js_objv(methods[M_NOOP]));
    js_set(g, "innerWidth", js_num(1280));
    js_set(g, "innerHeight", js_num(800));
    js_set(g, "devicePixelRatio", js_num(1));
    js_set(g, "scrollY", js_num(0));
    js_set(g, "pageYOffset", js_num(0));
}

/* Runs the window/document listeners for type (after the scripts). */
static void fire_global(const char *type){
    js_obj_t *ev = js_new_object();
    js_set(ev, "type", js_str(type));
    js_set(ev, "preventDefault", js_prevent_default_fn());
    js_val_t evv = js_objv(ev);
    for(int i = 0; i < nlisteners; i++){
        if(listeners[i].n || strcmp(listeners[i].type, type) != 0) continue;
        js_call(listeners[i].fn, js_objv(js_global()), 1, &evv);
    }
}

void dom_run_scripts(void){
    js_err[0] = 0;
    js_init();
    void *pool; u32 pool_n;
    web_pool_range(&pool, &pool_n);
    js_gc_root(pool, pool_n);
    js_gc_root(listeners, sizeof listeners);
    js_gc_root(&document, sizeof document);
    js_gc_root(methods, sizeof methods);
    setup_window();
    document = js_new_object();
    js_set_hooks(document, doc_get, NULL, NULL);
    js_set(document, "addEventListener", js_get(js_global(), "addEventListener"));
    js_set(document, "removeEventListener", js_objv(methods[M_NOOP]));
    js_set(js_global(), "document", js_objv(document));
    /* each <script> runs on its own: one that fails does not stop the others */
    for(int i = 0; i < web_nscripts && web_js; i++){
        if(web_script_len[i] == 0) continue;
        if(js_run(web_js + web_script_start[i], web_script_len[i]) != 0){
            const char *e = js_error();
            u32 k = 0;
            while(e[k] && k + 1 < sizeof js_err){ js_err[k] = e[k]; k++; }
            js_err[k] = 0;
        }
    }
    if(web_root) attach_attributes(web_root);
    fire_global("DOMContentLoaded");
    fire_global("load");
    dirty = 1;
}
