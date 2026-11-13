/* jslib.inc — the standard library: Function, Object, Error, Symbol, Array, String,
 * Number, Math, Date, RegExp, Promise, Map/Set, typed arrays and global functions.
 * Included at the end of js.c (it uses the interpreter's internals). */

#define NATIVE(name) static js_val_t name(js_val_t self, int argc, js_val_t *argv, void *ctx)
static void put_fn(js_obj_t *o, const char *name, js_native_fn fn, void *ctx){
    js_set(o, name, js_objv(js_new_native(fn, ctx)));
    find_own(o, name)->flags = PF_HIDDEN;           /* built-in methods are not enumerable */
}
static void put_val(js_obj_t *o, const char *name, js_val_t v){
    js_set(o, name, v);
    find_own(o, name)->flags = PF_HIDDEN | PF_RO;
}
static js_obj_t *ctor_named(const char *name, js_native_fn fn, js_obj_t *proto){
    js_obj_t *c = js_new_native(fn, NULL);
    if(proto){
        js_set(c, "prototype", js_objv(proto)); find_own(c, "prototype")->flags = PF_HIDDEN;
        js_set(proto, "constructor", js_objv(c)); find_own(proto, "constructor")->flags = PF_HIDDEN;
    }
    js_set(c, "name", js_str(name)); find_own(c, "name")->flags = PF_HIDDEN;
    js_set(global_obj, name, js_objv(c)); find_own(global_obj, name)->flags = PF_HIDDEN;
    return c;
}
static js_obj_t *new_with_proto(js_obj_t *proto){ js_obj_t *o = new_obj(); o->proto = proto; return o; }
/* self, when a constructor was called with new and self has the right prototype */
static js_obj_t *ctor_self(js_val_t self, js_obj_t *proto){
    if(self.t == JS_OBJ && self.o != global_obj){
        for(js_obj_t *x = self.o->proto; x; x = x->proto) if(x == proto) return self.o;
    }
    return NULL;
}
static double argn(int argc, js_val_t *argv, int i){ return i < argc ? js_to_num(argv[i]) : nan_value(); }
static int argi(int argc, js_val_t *argv, int i, int dflt){
    if(i >= argc || argv[i].t == JS_UNDEF) return dflt;
    double d = js_to_num(argv[i]);
    if(d != d) return 0;
    if(d > 2147483647.0) return 2147483647;
    if(d < -2147483647.0) return -2147483647;
    return (int)d;
}
static const char *num_str(double d){ char b[40]; kfmt_double(b, sizeof b, d); return jstrdup(b, (u32)strlen(b)); }
static const char *idx_str(int i){ char b[16]; kfmt_double(b, sizeof b, (double)i); return jstrdup(b, (u32)strlen(b)); }

/* ---- exceptions ---- */
static js_obj_t *error_protos[8];   /* Error, TypeError, RangeError, SyntaxError, ReferenceError, EvalError, URIError, AggregateError */
static const char *error_names[8] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError", "AggregateError" };
static js_val_t make_error(int type, const char *msg){
    js_obj_t *e = new_with_proto(error_protos[type] ? error_protos[type] : object_proto);
    e->kind = K_ERROR;
    js_set(e, "message", js_str(msg)); find_own(e, "message")->flags = PF_HIDDEN;
    u32 nl = (u32)strlen(error_names[type]), ml = (u32)strlen(msg);
    char *st = jalloc(nl + ml + 24);
    memcpy(st, error_names[type], nl); memcpy(st + nl, ": ", 2); memcpy(st + nl + 2, msg, ml);
    memcpy(st + nl + 2 + ml, "\n    at <anonymous>", 19); st[nl + ml + 21] = 0;
    js_set(e, "stack", js_str(st)); find_own(e, "stack")->flags = PF_HIDDEN;
    return js_objv(e);
}
/* the engine's runtime failures, as the Error a script can catch */
static js_val_t make_error_for_msg(const char *msg){
    const char *m = jstrdup(msg, (u32)strlen(msg));
    int type = 1;                                         /* TypeError */
    u32 L = (u32)strlen(m);
    if(L > 15 && strcmp(m + L - 15, " is not defined") == 0) type = 4;
    else if(strstr(m, "recursion") || strstr(m, "too long")) type = 2;
    return make_error(type, m);
}
static js_val_t throw_val(js_val_t v){ if(!STOP()){ thrown_val = v; thrown = 1; } return js_undef(); }
static js_val_t throw_type(const char *msg){ return throw_val(make_error(1, msg)); }
/* Takes a pending exception (thrown value or runtime failure). 1 if there was one. */
static int take_exception(js_val_t *out){
    if(thrown){ *out = thrown_val; thrown = 0; return 1; }
    if(failed){ *out = make_error_for_msg(err_msg); failed = 0; err_msg[0] = 0; return 1; }
    return 0;
}
NATIVE(error_ctor){
    int type = (int)(uintptr_t)ctx;
    js_obj_t *e = ctor_self(self, error_protos[type]);
    const char *msg = argc > 0 && argv[0].t != JS_UNDEF ? js_to_str(argv[0]) : "";
    js_val_t made = make_error(type, msg);
    if(e){
        e->kind = K_ERROR;
        for(prop_t *p = made.o->props; p; p = p->next){ js_set(e, p->k, p->v); find_own(e, p->k)->flags = PF_HIDDEN; }
        if(argc > 1 && argv[1].t == JS_OBJ){ js_val_t c = js_get(argv[1].o, "cause"); if(c.t != JS_UNDEF) js_set(e, "cause", c); }
        return js_objv(e);
    }
    return made;
}
NATIVE(error_to_string){
    if(self.t != JS_OBJ) return js_str("Error");
    js_val_t n = js_get(self.o, "name"), m = js_get(self.o, "message");
    const char *ns = n.t == JS_UNDEF ? "Error" : js_to_str(n), *ms = m.t == JS_UNDEF ? "" : js_to_str(m);
    if(!ms[0]) return js_str(ns);
    if(!ns[0]) return js_str(ms);
    u32 a = (u32)strlen(ns), b = (u32)strlen(ms);
    char *r = jalloc(a + b + 3); memcpy(r, ns, a); r[a] = ':'; r[a+1] = ' '; memcpy(r + a + 2, ms, b + 1);
    return js_str(r);
}
NATIVE(error_capture_stack){ (void)self; (void)ctx; if(argc > 0 && argv[0].t == JS_OBJ) js_set(argv[0].o, "stack", js_str("")); return js_undef(); }

/* ---- Function ---- */
typedef struct { js_val_t fn, self; js_obj_t *args; } bound_t;
NATIVE(fn_call){
    (void)ctx;
    if(!js_callable(self)) return throw_type("Function.prototype.call called on a non-function");
    return call_value(self, argc > 0 ? argv[0] : js_undef(), argc > 0 ? argc - 1 : 0, argv + 1);
}
/* Calls fn with the items of an array-like as its arguments (any number of them). */
static js_val_t call_spread(js_val_t fn, js_val_t th, js_val_t list){
    if(list.t != JS_OBJ) return call_value(fn, th, 0, NULL);
    int n;
    if(list.o->is_array) n = list.o->nitems;
    else { double d = js_to_num(js_get(list.o, "length")); n = d == d && d > 0 ? (int)d : 0; }
    if(n > 65536) n = 65536;
    js_val_t *a = n ? jalloc(sizeof(js_val_t) * (u32)n) : NULL;
    for(int i = 0; i < n; i++) a[i] = list.o->is_array ? list.o->items[i] : js_get(list.o, idx_str(i));
    js_obj_t *keep = list.o; (void)keep;
    return call_value(fn, th, n, a);
}
NATIVE(fn_apply){
    (void)ctx;
    if(!js_callable(self)) return throw_type("Function.prototype.apply called on a non-function");
    return call_spread(self, argc > 0 ? argv[0] : js_undef(), argc > 1 ? argv[1] : js_undef());
}
NATIVE(bound_call){
    bound_t *b = ctx;
    int n = b->args ? b->args->nitems : 0;
    js_val_t *a = jalloc(sizeof(js_val_t) * (u32)(n + argc + 1));
    for(int i = 0; i < n; i++) a[i] = b->args->items[i];
    for(int i = 0; i < argc; i++) a[n + i] = argv[i];
    /* new on a bound function constructs the target */
    js_val_t th = b->self;
    if(self.t == JS_OBJ && self.o != global_obj && b->fn.t == JS_OBJ){
        js_obj_t *pt = prototype_of(b->fn.o);
        if(pt && self.o->proto == pt) th = self;
    }
    return call_value(b->fn, th, n + argc, a);
}
NATIVE(fn_bind){
    (void)ctx;
    if(!js_callable(self)) return throw_type("Bind must be called on a function");
    bound_t *b = jalloc(sizeof(bound_t));
    b->fn = self;
    b->self = argc > 0 ? argv[0] : js_undef();
    b->args = NULL;
    if(argc > 1){ b->args = new_array(); for(int i = 1; i < argc; i++) arr_push(b->args, argv[i]); }
    js_obj_t *f = js_new_native(bound_call, b);
    js_val_t pt = js_get(self.o, "prototype");
    if(pt.t == JS_OBJ){ js_set(f, "prototype", pt); find_own(f, "prototype")->flags = PF_HIDDEN; }
    return js_objv(f);
}
NATIVE(fn_to_string){
    (void)ctx; (void)argc; (void)argv;
    if(self.t == JS_OBJ && self.o->is_func){
        if(self.o->native) return js_str("function () { [native code] }");
        return js_str("function () { [code] }");
    }
    return throw_type("Function.prototype.toString requires a function");
}
NATIVE(fn_noop){ (void)self; (void)argc; (void)argv; (void)ctx; return js_undef(); }
/* new Function("a", "b", "return a + b") */
static js_val_t compile_fn_src(const char *params, const char *body){
    u32 pl = (u32)strlen(params), bl = (u32)strlen(body);
    char *src = jalloc(pl + bl + 40);
    u32 k = 0;
    const char *h = "(function anonymous(";
    for(const char *q = h; *q; q++) src[k++] = *q;
    memcpy(src + k, params, pl); k += pl;
    const char *m = "\n) {\n";
    for(const char *q = m; *q; q++) src[k++] = *q;
    memcpy(src + k, body, bl); k += bl;
    src[k++] = '\n'; src[k++] = '}'; src[k++] = ')'; src[k] = 0;
    tok_t *st = toks; int sn = ntok, sti = ti, slazy = lazy_on, snoin = no_in; u32 scap = tok_cap;
    lazy_on = 0; no_in = 0;
    tok_push();
    lex(src, k);
    ti = 0;
    jsnode_t *e = STOP() ? NULL : expression();
    tok_pop();
    toks = st; ntok = sn; ti = sti; tok_cap = scap; lazy_on = slazy; no_in = snoin;
    if(STOP() || !e){ if(failed){ failed = 0; return throw_val(make_error(3, err_msg)); } return js_undef(); }
    return eval(e, global_env);
}
NATIVE(function_ctor){
    (void)self; (void)ctx;
    if(argc == 0) return compile_fn_src("", "");
    u32 total = 0;
    for(int i = 0; i + 1 < argc; i++) total += (u32)strlen(js_to_str(argv[i])) + 1;
    char *ps = jalloc(total + 1); u32 k = 0;
    for(int i = 0; i + 1 < argc; i++){
        const char *a = js_to_str(argv[i]); u32 l = (u32)strlen(a);
        if(i) ps[k++] = ',';
        memcpy(ps + k, a, l); k += l;
    }
    ps[k] = 0;
    return compile_fn_src(ps, js_to_str(argv[argc - 1]));
}

/* ---- Symbol: a symbol is a string that starts with \x01 ---- */
static int symbol_count = 0;
static js_val_t make_symbol(const char *desc){
    char b[120]; u32 k = 0;
    b[k++] = '\x01';
    for(const char *q = "Symbol("; *q; q++) b[k++] = *q;
    for(const char *q = desc; *q && k < 96; q++) b[k++] = *q;
    b[k++] = ')'; b[k++] = '#';
    int n = ++symbol_count;
    char t[12]; int tk = 0; do { t[tk++] = (char)('0' + n % 10); n /= 10; } while(n);
    while(tk) b[k++] = t[--tk];
    return js_str(jstrdup(b, k));
}
static js_obj_t *symbol_registry = NULL;
NATIVE(symbol_fn){ (void)self; (void)ctx; return make_symbol(argc > 0 && argv[0].t != JS_UNDEF ? js_to_str(argv[0]) : ""); }
NATIVE(symbol_for){
    (void)self; (void)ctx;
    const char *k = argc > 0 ? js_to_str(argv[0]) : "undefined";
    js_val_t v = js_get(symbol_registry, k);
    if(v.t == JS_UNDEF){ v = make_symbol(k); js_set(symbol_registry, k, v); }
    return v;
}
NATIVE(symbol_to_string){
    (void)ctx; (void)argc; (void)argv;
    if(self.t != JS_STR || self.s[0] != '\x01') return js_str("Symbol()");
    const char *s = self.s + 1; const char *h = strchr(s, '#');
    return js_str(jstrdup(s, h ? (u32)(h - s) : (u32)strlen(s)));
}
#define SYM_ITERATOR "\x01Symbol(Symbol.iterator)"
#define SYM_ASYNC_ITERATOR "\x01Symbol(Symbol.asyncIterator)"
#define SYM_TO_STRING_TAG "\x01Symbol(Symbol.toStringTag)"
#define SYM_HAS_INSTANCE "\x01Symbol(Symbol.hasInstance)"
#define SYM_TO_PRIMITIVE "\x01Symbol(Symbol.toPrimitive)"

/* ---- Object ---- */
static js_obj_t *to_object(js_val_t v){
    if(v.t == JS_OBJ) return v.o;
    js_obj_t *o = NULL;
    if(v.t == JS_STR){ o = new_with_proto(string_proto); o->kind = K_STRING; }
    else if(v.t == JS_NUM){ o = new_with_proto(number_proto); o->kind = K_NUMBER; }
    else if(v.t == JS_BOOL){ o = new_with_proto(bool_proto); o->kind = K_BOOL; }
    else return js_new_object();
    js_val_t *slot = jalloc(sizeof(js_val_t)); *slot = v; o->slot = slot;
    return o;
}
NATIVE(object_ctor){
    (void)self; (void)ctx;
    if(argc == 0 || argv[0].t == JS_UNDEF || argv[0].t == JS_NULL) return js_objv(js_new_object());
    return js_objv(to_object(argv[0]));
}
static int own_has(js_obj_t *o, const char *k){
    if(find_own(o, k)) return 1;
    if(o->is_array){ if(strcmp(k, "length") == 0) return 1; int i = as_index(k); if(i >= 0 && i < o->nitems) return 1; }
    if(o->hget){ js_val_t t; if(o->hget(o, k, &t)) return 1; }
    return 0;
}
NATIVE(obj_has_own){
    (void)ctx;
    if(argc < 1) return js_bool(0);
    if(self.t == JS_STR){ int i = as_index(js_to_str(argv[0])); return js_bool(i >= 0 && i < (int)strlen(self.s)); }
    if(self.t != JS_OBJ) return js_bool(0);
    return js_bool(own_has(self.o, js_to_str(argv[0])));
}
NATIVE(obj_has_own_static){
    (void)self; (void)ctx;
    if(argc < 2 || argv[0].t != JS_OBJ) return js_bool(0);
    return js_bool(own_has(argv[0].o, js_to_str(argv[1])));
}
static const char *class_of(js_val_t v){
    switch(v.t){
        case JS_UNDEF: return "Undefined"; case JS_NULL: return "Null"; case JS_BOOL: return "Boolean";
        case JS_NUM: return "Number"; case JS_STR: return "String";
    }
    js_obj_t *o = v.o;
    js_val_t tag = js_get(o, SYM_TO_STRING_TAG);
    if(tag.t == JS_STR) return tag.s;
    if(o->is_array) return "Array";
    if(o->is_func) return "Function";
    if(o->is_regex) return "RegExp";
    switch(o->kind){
        case K_DATE: return "Date"; case K_ERROR: return "Error"; case K_BOOL: return "Boolean";
        case K_NUMBER: return "Number"; case K_STRING: return "String";
    }
    if(o->hget && o == global_obj) return "Window";
    return "Object";
}
NATIVE(obj_to_string){
    (void)ctx; (void)argc; (void)argv;
    const char *c = class_of(self);
    u32 l = (u32)strlen(c);
    char *r = jalloc(l + 10);
    memcpy(r, "[object ", 8); memcpy(r + 8, c, l); r[8 + l] = ']'; r[9 + l] = 0;
    return js_str(r);
}
NATIVE(obj_value_of){
    (void)ctx; (void)argc; (void)argv;
    if(self.t == JS_OBJ && self.o->slot && (self.o->kind == K_NUMBER || self.o->kind == K_STRING || self.o->kind == K_BOOL))
        return *(js_val_t *)self.o->slot;
    return self;
}
NATIVE(obj_is_proto_of){
    (void)ctx;
    if(argc < 1 || argv[0].t != JS_OBJ || self.t != JS_OBJ) return js_bool(0);
    for(js_obj_t *x = argv[0].o->proto; x; x = x->proto) if(x == self.o) return js_bool(1);
    return js_bool(0);
}
NATIVE(obj_prop_is_enum){
    (void)ctx;
    if(argc < 1 || self.t != JS_OBJ) return js_bool(0);
    const char *k = js_to_str(argv[0]);
    prop_t *p = find_own(self.o, k);
    if(p) return js_bool(!(p->flags & PF_HIDDEN));
    if(self.o->is_array){ int i = as_index(k); return js_bool(i >= 0 && i < self.o->nitems); }
    return js_bool(0);
}
/* applies a property descriptor */
static void define_prop(js_obj_t *o, const char *k, js_obj_t *d){
    if(o->is_array){
        int i = as_index(k);
        if(i >= 0 && has_prop(d, "value")){ obj_set(o, k, js_get(d, "value")); return; }
    }
    prop_t *p = find_own(o, k);
    int existed = p != NULL;
    if(!p){ js_set(o, k, js_undef()); p = find_own(o, k); }
    js_val_t g = js_get(d, "get"), st = js_get(d, "set");
    if(g.t == JS_OBJ && g.o->is_func){ p->getter = g.o; }
    if(st.t == JS_OBJ && st.o->is_func){ p->setter = st.o; }
    if(has_prop(d, "value")){ p->v = js_get(d, "value"); p->getter = p->setter = NULL; }
    int flags = existed ? p->flags : (PF_HIDDEN | PF_RO);
    if(has_prop(d, "enumerable")){ if(js_truthy(js_get(d, "enumerable"))) flags &= ~PF_HIDDEN; else flags |= PF_HIDDEN; }
    if(has_prop(d, "writable")){ if(js_truthy(js_get(d, "writable"))) flags &= ~PF_RO; else flags |= PF_RO; }
    else if(p->getter || p->setter) flags &= ~PF_RO;
    p->flags = flags;
}
NATIVE(obj_define_property){
    (void)self; (void)ctx;
    if(argc < 3 || argv[0].t != JS_OBJ) return throw_type("Object.defineProperty called on non-object");
    if(argv[2].t != JS_OBJ) return throw_type("Property description must be an object");
    define_prop(argv[0].o, js_to_str(argv[1]), argv[2].o);
    return argv[0];
}
NATIVE(obj_define_properties){
    (void)self; (void)ctx;
    if(argc < 2 || argv[0].t != JS_OBJ || argv[1].t != JS_OBJ) return argc ? argv[0] : js_undef();
    js_obj_t *ks = keys_of(argv[1]);
    for(int i = 0; i < ks->nitems; i++){
        js_val_t d = js_get(argv[1].o, ks->items[i].s);
        if(d.t == JS_OBJ) define_prop(argv[0].o, ks->items[i].s, d.o);
    }
    return argv[0];
}
NATIVE(obj_get_own_desc){
    (void)self; (void)ctx;
    if(argc < 2 || argv[0].t != JS_OBJ) return js_undef();
    js_obj_t *o = argv[0].o; const char *k = js_to_str(argv[1]);
    prop_t *p = find_own(o, k);
    js_obj_t *d = js_new_object();
    if(!p){
        if(!own_has(o, k)) return js_undef();
        js_set(d, "value", js_get(o, k)); js_set(d, "writable", js_bool(1));
        js_set(d, "enumerable", js_bool(1)); js_set(d, "configurable", js_bool(1));
        return js_objv(d);
    }
    if(p->getter || p->setter){
        js_set(d, "get", p->getter ? js_objv(p->getter) : js_undef());
        js_set(d, "set", p->setter ? js_objv(p->setter) : js_undef());
    } else { js_set(d, "value", p->v); js_set(d, "writable", js_bool(!(p->flags & PF_RO))); }
    js_set(d, "enumerable", js_bool(!(p->flags & PF_HIDDEN)));
    js_set(d, "configurable", js_bool(1));
    return js_objv(d);
}
NATIVE(obj_get_own_names){
    (void)self; (void)ctx;
    js_obj_t *out = new_array();
    if(argc < 1 || argv[0].t != JS_OBJ) return js_objv(out);
    js_obj_t *o = argv[0].o;
    if(o->is_array){ for(int i = 0; i < o->nitems; i++) arr_push(out, js_str(idx_str(i))); arr_push(out, js_str("length")); }
    int n = 0;
    for(prop_t *p = o->props; p; p = p->next) if(p->k[0] != '\x01') n++;
    const char **tmp = n ? jalloc(sizeof(char *) * (u32)n) : NULL;
    int k = n;
    for(prop_t *p = o->props; p; p = p->next) if(p->k[0] != '\x01') tmp[--k] = p->k;
    for(int i = 0; i < n; i++) arr_push(out, js_str(tmp[i]));
    return js_objv(out);
}
NATIVE(obj_get_own_symbols){
    (void)self; (void)ctx;
    js_obj_t *out = new_array();
    if(argc < 1 || argv[0].t != JS_OBJ) return js_objv(out);
    for(prop_t *p = argv[0].o->props; p; p = p->next) if(p->k[0] == '\x01') arr_push(out, js_str(p->k));
    return js_objv(out);
}
NATIVE(obj_set_proto){
    (void)self; (void)ctx;
    if(argc < 2 || argv[0].t != JS_OBJ) return argc ? argv[0] : js_undef();
    argv[0].o->proto = argv[1].t == JS_OBJ ? argv[1].o : NULL;
    return argv[0];
}
NATIVE(obj_get_proto2){
    (void)self; (void)ctx;
    if(argc < 1) return js_null();
    js_val_t v = argv[0];
    if(v.t == JS_STR) return js_objv(string_proto);
    if(v.t == JS_NUM) return js_objv(number_proto);
    if(v.t == JS_BOOL) return js_objv(bool_proto);
    if(v.t != JS_OBJ) return throw_type("Cannot convert undefined or null to object");
    return v.o->proto ? js_objv(v.o->proto) : js_null();
}
NATIVE(obj_is){
    (void)self; (void)ctx;
    js_val_t a = argc > 0 ? argv[0] : js_undef(), b = argc > 1 ? argv[1] : js_undef();
    if(a.t == JS_NUM && b.t == JS_NUM){
        if(a.n != a.n && b.n != b.n) return js_bool(1);
        if(a.n == 0 && b.n == 0) return js_bool((1.0 / a.n > 0) == (1.0 / b.n > 0));
    }
    return js_bool(same(a, b));
}
NATIVE(obj_seal){ (void)self; (void)ctx; return argc ? argv[0] : js_undef(); }
NATIVE(obj_true){ (void)self; (void)ctx; (void)argc; (void)argv; return js_bool(1); }
NATIVE(obj_false){ (void)self; (void)ctx; (void)argc; (void)argv; return js_bool(0); }
NATIVE(obj_create2){
    (void)self; (void)ctx;
    js_obj_t *o = new_obj();
    o->proto = argc > 0 && argv[0].t == JS_OBJ ? argv[0].o : NULL;
    if(argc > 1 && argv[1].t == JS_OBJ){ js_val_t a[2] = { js_objv(o), argv[1] }; obj_define_properties(js_undef(), 2, a, NULL); }
    return js_objv(o);
}
NATIVE(proto_getter){ (void)ctx; (void)argc; (void)argv; return self.t == JS_OBJ && self.o->proto ? js_objv(self.o->proto) : js_null(); }
NATIVE(proto_setter){ (void)ctx; if(self.t == JS_OBJ && argc > 0) self.o->proto = argv[0].t == JS_OBJ ? argv[0].o : NULL; return js_undef(); }

/* ---- iteration: arrays, strings, Map/Set, and objects with [Symbol.iterator] ---- */
static js_obj_t *map_items(js_obj_t *m, int what);   /* Map/Set contents as an array */
/* The values a for-of loop (or ...spread, Array.from) visits, as an array. */
static js_obj_t *iterate_to_array(js_val_t v){
    js_obj_t *out = new_array();
    if(v.t == JS_STR){
        for(const unsigned char *p = (const unsigned char *)v.s; *p; ){
            u32 n = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
            u32 i = 0; while(i < n && p[i]) i++;
            arr_push(out, js_str(jstrdup((const char *)p, i)));
            p += i;
        }
        return out;
    }
    if(v.t != JS_OBJ) return out;
    if(v.o->is_array) return v.o;
    if(v.o->kind == K_MAP) return map_items(v.o, 2);
    if(v.o->kind == K_SET) return map_items(v.o, 1);
    if(v.o->kind == K_TYPED){ for(int i = 0; i < v.o->nitems; i++) arr_push(out, v.o->items[i]); return out; }
    js_val_t f = js_get(v.o, SYM_ITERATOR);
    js_val_t it = v;
    if(js_callable(f)){ it = call_value(f, v, 0, NULL); if(STOP()) return out; }
    if(it.t != JS_OBJ) return out;
    js_val_t next = js_get(it.o, "next");
    if(!js_callable(next)){
        /* array-like: { length, 0, 1, ... } */
        double L = js_to_num(js_get(v.o, "length"));
        if(L == L) for(int i = 0; i < (int)L && i < 10000000; i++) arr_push(out, js_get(v.o, idx_str(i)));
        return out;
    }
    for(int guard = 0; guard < 10000000 && !STOP(); guard++){
        js_val_t r = call_value(next, it, 0, NULL);
        if(STOP() || r.t != JS_OBJ) break;
        if(js_truthy(js_get(r.o, "done"))) break;
        arr_push(out, js_get(r.o, "value"));
    }
    return out;
}
/* an iterator object over an array's values: { next(), [Symbol.iterator]() } */
typedef struct { js_obj_t *arr; int i; } arr_iter_t;
NATIVE(arr_iter_next){
    (void)self; (void)argc; (void)argv;
    arr_iter_t *it = ctx;
    js_obj_t *r = js_new_object();
    if(it->i < it->arr->nitems){ js_set(r, "value", it->arr->items[it->i++]); js_set(r, "done", js_bool(0)); }
    else { js_set(r, "value", js_undef()); js_set(r, "done", js_bool(1)); }
    return js_objv(r);
}
NATIVE(return_self){ (void)ctx; (void)argc; (void)argv; return self; }
static js_obj_t *iter_proto = NULL;
static js_val_t make_array_iter(js_obj_t *arr){
    arr_iter_t *st = jalloc(sizeof(arr_iter_t));
    st->arr = arr; st->i = 0;
    js_obj_t *it = new_with_proto(iter_proto);
    it->kind = K_ITER; it->slot = st;
    js_set(it, "next", js_objv(js_new_native(arr_iter_next, st)));
    find_own(it, "next")->flags = PF_HIDDEN;
    return js_objv(it);
}
NATIVE(arr_values){ (void)ctx; (void)argc; (void)argv; return make_array_iter(iterate_to_array(self)); }
NATIVE(arr_keys){
    (void)ctx; (void)argc; (void)argv;
    js_obj_t *a = new_array();
    int n = self.t == JS_OBJ && self.o->is_array ? self.o->nitems : 0;
    for(int i = 0; i < n; i++) arr_push(a, js_num(i));
    return make_array_iter(a);
}
NATIVE(arr_entries){
    (void)ctx; (void)argc; (void)argv;
    js_obj_t *a = new_array();
    if(self.t == JS_OBJ && self.o->is_array)
        for(int i = 0; i < self.o->nitems; i++){ js_obj_t *e = new_array(); arr_push(e, js_num(i)); arr_push(e, self.o->items[i]); arr_push(a, js_objv(e)); }
    return make_array_iter(a);
}

/* ---- a growable string ---- */
typedef struct { char *b; u32 n, cap; } sb_t;
static void sb_init(sb_t *s, u32 cap){ s->cap = cap < 32 ? 32 : cap; s->b = jalloc(s->cap); s->n = 0; s->b[0] = 0; }
static void sb_putn(sb_t *s, const char *t, u32 n){
    if(s->n + n + 1 > s->cap){
        u32 nc = s->cap * 2; while(nc < s->n + n + 1) nc *= 2;
        char *nb = jalloc(nc);
        if((char *)nb == scratch) return;
        memcpy(nb, s->b, s->n); s->b = nb; s->cap = nc;
    }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}
static void sb_puts(sb_t *s, const char *t){ sb_putn(s, t, (u32)strlen(t)); }
static void sb_putc(sb_t *s, char c){ sb_putn(s, &c, 1); }

/* ---- Array ---- */
NATIVE(array_ctor){
    (void)self; (void)ctx;
    js_obj_t *a = new_array();
    if(argc == 1 && argv[0].t == JS_NUM){
        double n = argv[0].n;
        if(n < 0 || n != (double)(u32)n){ return throw_val(make_error(2, "Invalid array length")); }
        for(u32 i = 0; i < (u32)n && i < 50000000u; i++) arr_push(a, js_hole());
        return js_objv(a);
    }
    for(int i = 0; i < argc; i++) arr_push(a, argv[i]);
    return js_objv(a);
}
NATIVE(arr_join2){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return js_str("");
    const char *sep = argc && argv[0].t != JS_UNDEF ? js_to_str(argv[0]) : ",";
    sb_t s; sb_init(&s, 64);
    for(int i = 0; i < self.o->nitems && !STOP(); i++){
        if(i) sb_puts(&s, sep);
        js_val_t v = self.o->items[i];
        if(v.t != JS_UNDEF && v.t != JS_NULL) sb_puts(&s, js_to_str(v));
    }
    return js_str(s.b);
}
NATIVE(arr_to_string){ (void)ctx; (void)argc; (void)argv; return arr_join2(self, 0, NULL, NULL); }
NATIVE(arr_splice){
    (void)ctx;
    js_obj_t *removed = new_array();
    if(self.t != JS_OBJ || !self.o->is_array) return js_objv(removed);
    js_obj_t *a = self.o;
    int n = a->nitems;
    int start = argi(argc, argv, 0, 0);
    if(start < 0) start += n; if(start < 0) start = 0; if(start > n) start = n;
    int del = argc < 2 ? n - start : argi(argc, argv, 1, 0);
    if(del < 0) del = 0; if(del > n - start) del = n - start;
    int add = argc > 2 ? argc - 2 : 0;
    for(int i = 0; i < del; i++) arr_push(removed, a->items[start + i]);
    int nn = n - del + add;
    while(a->nitems < nn) arr_push(a, js_undef());
    if(add < del){
        for(int i = start + add; i < nn; i++) a->items[i] = a->items[i + del - add];
    } else if(add > del){
        for(int i = nn - 1; i >= start + add; i--) a->items[i] = a->items[i - add + del];
    }
    for(int i = 0; i < add; i++) a->items[start + i] = argv[2 + i];
    a->nitems = nn;
    return js_objv(removed);
}
NATIVE(arr_last_index_of){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_num(-1);
    int from = argc > 1 ? argi(argc, argv, 1, 0) : self.o->nitems - 1;
    if(from < 0) from += self.o->nitems;
    if(from >= self.o->nitems) from = self.o->nitems - 1;
    for(int i = from; i >= 0; i--) if(same(self.o->items[i], argv[0])) return js_num(i);
    return js_num(-1);
}
NATIVE(arr_reduce_right){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return js_undef();
    int i = self.o->nitems - 1;
    js_val_t acc;
    if(argc > 1) acc = argv[1];
    else { if(i < 0) return throw_type("Reduce of empty array with no initial value"); acc = self.o->items[i--]; }
    for(; i >= 0 && !STOP(); i--){
        js_val_t a[4] = { acc, self.o->items[i], js_num(i), self };
        acc = call_value(argv[0], js_undef(), 4, a);
    }
    return acc;
}
NATIVE(arr_find_last){
    int want_index = ctx != NULL;
    if(self.t != JS_OBJ || !self.o->is_array || argc < 1) return want_index ? js_num(-1) : js_undef();
    for(int i = self.o->nitems - 1; i >= 0 && !STOP(); i--){
        js_val_t a[3] = { self.o->items[i], js_num(i), self };
        if(js_truthy(call_value(argv[0], argc > 1 ? argv[1] : js_undef(), 3, a))) return want_index ? js_num(i) : self.o->items[i];
    }
    return want_index ? js_num(-1) : js_undef();
}
NATIVE(arr_copy_within){
    (void)ctx;
    if(self.t != JS_OBJ || !self.o->is_array) return self;
    int n = self.o->nitems;
    int t = argi(argc, argv, 0, 0), s = argi(argc, argv, 1, 0), e = argi(argc, argv, 2, n);
    if(t < 0) t += n; if(s < 0) s += n; if(e < 0) e += n;
    if(t < 0) t = 0; if(s < 0) s = 0; if(e > n) e = n;
    int cnt = e - s; if(cnt > n - t) cnt = n - t;
    if(cnt <= 0) return self;
    js_val_t *tmp = jalloc(sizeof(js_val_t) * (u32)cnt);
    for(int i = 0; i < cnt; i++) tmp[i] = self.o->items[s + i];
    for(int i = 0; i < cnt; i++) self.o->items[t + i] = tmp[i];
    return self;
}
NATIVE(arr_from2){
    (void)self; (void)ctx;
    js_obj_t *out = new_array();
    if(argc < 1 || argv[0].t == JS_UNDEF || argv[0].t == JS_NULL) return js_objv(out);
    js_obj_t *src;
    if(argv[0].t == JS_OBJ && !argv[0].o->is_array && argv[0].o->kind != K_MAP && argv[0].o->kind != K_SET &&
       !js_callable(js_get(argv[0].o, SYM_ITERATOR)) && argv[0].o->kind != K_TYPED){
        src = new_array();
        double L = js_to_num(js_get(argv[0].o, "length"));
        if(L == L) for(int i = 0; i < (int)L && i < 50000000; i++) arr_push(src, js_get(argv[0].o, idx_str(i)));
    } else src = iterate_to_array(argv[0]);
    js_val_t fn = argc > 1 ? argv[1] : js_undef();
    for(int i = 0; i < src->nitems && !STOP(); i++){
        js_val_t v = src->items[i];
        if(js_callable(fn)){ js_val_t a[2] = { v, js_num(i) }; v = call_value(fn, argc > 2 ? argv[2] : js_undef(), 2, a); }
        arr_push(out, v);
    }
    return js_objv(out);
}

/* ---- String ---- */
static const char *this_str(js_val_t self){
    if(self.t == JS_OBJ && self.o->kind == K_STRING && self.o->slot) return ((js_val_t *)self.o->slot)->s;
    return js_to_str(self);
}
NATIVE(str_last_index_of){
    (void)ctx;
    const char *s = this_str(self), *t = argc ? js_to_str(argv[0]) : "undefined";
    int sl = (int)strlen(s), tl = (int)strlen(t);
    int from = argc > 1 && argv[1].t != JS_UNDEF ? argi(argc, argv, 1, sl) : sl;
    if(from > sl - tl) from = sl - tl;
    for(int i = from; i >= 0; i--) if(strncmp(s + i, t, (u32)tl) == 0) return js_num(i);
    return js_num(-1);
}
NATIVE(str_substr){
    (void)ctx;
    const char *s = this_str(self); int L = (int)strlen(s);
    int a = argi(argc, argv, 0, 0); if(a < 0) a += L; if(a < 0) a = 0; if(a > L) a = L;
    int n = argi(argc, argv, 1, L - a); if(n < 0) n = 0; if(n > L - a) n = L - a;
    return js_str(jstrdup(s + a, (u32)n));
}
NATIVE(str_code_point_at){
    (void)ctx;
    const unsigned char *s = (const unsigned char *)this_str(self);
    int i = argi(argc, argv, 0, 0), L = (int)strlen((const char *)s);
    if(i < 0 || i >= L) return js_undef();
    u32 c = s[i];
    if(c >= 0xF0 && i + 3 < L) c = ((c & 7) << 18) | ((s[i+1] & 63) << 12) | ((s[i+2] & 63) << 6) | (s[i+3] & 63);
    else if(c >= 0xE0 && i + 2 < L) c = ((c & 15) << 12) | ((s[i+1] & 63) << 6) | (s[i+2] & 63);
    else if(c >= 0xC0 && i + 1 < L) c = ((c & 31) << 6) | (s[i+1] & 63);
    return js_num(c);
}
NATIVE(str_self){ (void)ctx; (void)argc; (void)argv; return js_str(this_str(self)); }
NATIVE(str_locale_compare){
    (void)ctx;
    int c = strcmp(this_str(self), argc ? js_to_str(argv[0]) : "undefined");
    return js_num(c < 0 ? -1 : c > 0 ? 1 : 0);
}
NATIVE(str_concat){
    (void)ctx;
    sb_t s; sb_init(&s, 64); sb_puts(&s, this_str(self));
    for(int i = 0; i < argc; i++) sb_puts(&s, js_to_str(argv[i]));
    return js_str(s.b);
}
NATIVE(str_from_code_point){
    (void)self; (void)ctx;
    sb_t s; sb_init(&s, 16);
    for(int i = 0; i < argc; i++){ char b[4]; u32 n = put_utf8(b, (u32)js_to_num(argv[i])); sb_putn(&s, b, n); }
    return js_str(s.b);
}
NATIVE(str_from_char_code2){
    (void)self; (void)ctx;
    sb_t s; sb_init(&s, 16);
    for(int i = 0; i < argc; i++){
        u32 c = to_u32(js_to_num(argv[i])) & 0xFFFF;
        if(c >= 0xD800 && c < 0xDC00 && i + 1 < argc){
            u32 lo = to_u32(js_to_num(argv[i+1])) & 0xFFFF;
            if(lo >= 0xDC00 && lo < 0xE000){ c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        }
        char b[4]; u32 n = put_utf8(b, c); sb_putn(&s, b, n);
    }
    return js_str(s.b);
}

/* ---- Number, Boolean ---- */
NATIVE(num_is_integer){ (void)self; (void)ctx; if(argc < 1 || argv[0].t != JS_NUM) return js_bool(0); double d = argv[0].n; return js_bool(d == d && d - kfloor(d) == 0 && d != 1.0/0.0 && d != -1.0/0.0); }
NATIVE(num_is_safe_integer){ (void)self; (void)ctx; if(argc < 1 || argv[0].t != JS_NUM) return js_bool(0); double d = argv[0].n; return js_bool(d == d && d - kfloor(d) == 0 && kfabs(d) <= 9007199254740991.0); }
NATIVE(num_is_finite){ (void)self; (void)ctx; if(argc < 1 || argv[0].t != JS_NUM) return js_bool(0); double d = argv[0].n; return js_bool(d == d && d != 1.0/0.0 && d != -1.0/0.0); }
NATIVE(num_is_nan){ (void)self; (void)ctx; return js_bool(argc > 0 && argv[0].t == JS_NUM && argv[0].n != argv[0].n); }
NATIVE(g_is_finite){ (void)self; (void)ctx; double d = argn(argc, argv, 0); return js_bool(d == d && d != 1.0/0.0 && d != -1.0/0.0); }
NATIVE(num_value_of){
    (void)ctx; (void)argc; (void)argv;
    if(self.t == JS_OBJ && self.o->kind == K_NUMBER && self.o->slot) return *(js_val_t *)self.o->slot;
    return js_num(js_to_num(self));
}
NATIVE(num_to_precision){
    (void)ctx;
    double v = js_to_num(self);
    if(argc < 1 || argv[0].t == JS_UNDEF) return js_str(js_to_str(js_num(v)));
    int p = argi(argc, argv, 0, 6);
    if(v == 0 || v != v) return js_str(js_to_str(js_num(v)));
    double m = kfabs(v); int e = 0;
    while(m >= 10){ m /= 10; e++; } while(m < 1){ m *= 10; e--; }
    double scale = kpow(10, (double)(p - 1 - e));
    double r = kfloor(kfabs(v) * scale + 0.5) / scale;
    if(v < 0) r = -r;
    if(e >= p || e < -6) return js_str(js_to_str(js_num(r)));
    int dec = p - 1 - e; if(dec < 0) dec = 0;
    js_val_t a[1] = { js_num(dec) };
    return num_to_fixed(js_num(r), 1, a, NULL);
}
NATIVE(num_to_exponential){
    (void)ctx;
    double v = js_to_num(self);
    if(v != v) return js_str("NaN");
    if(v == 1.0/0.0) return js_str("Infinity");
    if(v == -1.0/0.0) return js_str("-Infinity");
    int auto_digits = argc < 1 || argv[0].t == JS_UNDEF;
    int f = auto_digits ? 0 : argi(argc, argv, 0, 0);
    if(f < 0 || f > 100) return throw_val(make_error(2, "toExponential() argument must be between 0 and 100"));
    int neg = v < 0; double a = neg ? -v : v;
    int e = 0;
    if(a != 0){ while(a >= 10){ a /= 10; e++; } while(a < 1){ a *= 10; e--; } }
    if(auto_digits){
        /* as many digits as the shortest round-trip form needs */
        const char *full = js_to_str(js_num(neg ? -v : v));
        int digs = 0, seen = 0;
        for(const char *q = full; *q && *q != 'e'; q++) if(*q >= '0' && *q <= '9'){ if(*q != '0' || seen){ seen = 1; digs++; } }
        const char *t = full; while(*t && *t != 'e') t++;
        /* trailing zeros of an integer are not significant */
        if(!*t){ const char *q = full + strlen(full) - 1; while(q > full && *q == '0' && !strchr(full, '.')){ digs--; q--; } }
        f = digs > 0 ? digs - 1 : 0;
        if(f > 20) f = 20;
    }
    double scale = kpow(10, f);
    double m = kfloor(a * scale + 0.5);
    if(m >= 10 * scale){ m /= 10; e++; }
    char digits[128]; int n = 0;
    double mm = m;
    char tmp[128]; int tn = 0;
    do { tmp[tn++] = (char)('0' + (int)kfmod(mm, 10)); mm = kfloor(mm / 10); } while(mm > 0 && tn < 120);
    while(tn < f + 1) tmp[tn++] = '0';
    sb_t o; sb_init(&o, 32);
    if(neg) sb_putc(&o, '-');
    for(int i = tn - 1; i >= 0; i--){ digits[n++] = tmp[i]; }
    sb_putc(&o, digits[0]);
    if(f > 0){ sb_putc(&o, '.'); sb_putn(&o, digits + 1, (u32)f); }
    sb_putc(&o, 'e'); sb_putc(&o, e < 0 ? '-' : '+');
    sb_puts(&o, num_str(e < 0 ? -e : e));
    return js_str(o.b);
}
NATIVE(bool_ctor){
    (void)ctx;
    int b = argc > 0 && js_truthy(argv[0]);
    js_obj_t *o = ctor_self(self, bool_proto);
    if(o){ js_val_t *slot = jalloc(sizeof(js_val_t)); *slot = js_bool(b); o->slot = slot; o->kind = K_BOOL; return js_objv(o); }
    return js_bool(b);
}
NATIVE(bool_to_string){
    (void)ctx; (void)argc; (void)argv;
    js_val_t v = self.t == JS_OBJ && self.o->slot ? *(js_val_t *)self.o->slot : self;
    return js_str(js_truthy(v) ? "true" : "false");
}

/* ---- Math ---- */
static double fn_sign(double x){ return x > 0 ? 1 : x < 0 ? -1 : x; }
NATIVE(math_fn2){
    (void)self;
    double x = argn(argc, argv, 0), y = argn(argc, argv, 1);
    switch((int)(uintptr_t)ctx){
        case 0: return js_num(x != x ? x : ktrunc(x));
        case 1: return js_num(fn_sign(x));
        case 2: return js_num(klog(x));
        case 3: return js_num(klog(x) / 0.6931471805599453);
        case 4: return js_num(klog(x) / 2.302585092994046);
        case 5: return js_num(kexp(x));
        case 6: return js_num(ksin(x));
        case 7: return js_num(kcos(x));
        case 8: return js_num(ktan(x));
        case 9: return js_num(katan(x));
        case 10: return js_num(katan2(x, y));
        case 11: return js_num(katan2(x, ksqrt(1 - x * x)));
        case 12: return js_num(katan2(ksqrt(1 - x * x), x));
        case 13: { double s = 0; for(int i = 0; i < argc; i++){ double v = js_to_num(argv[i]); s += v * v; } return js_num(ksqrt(s)); }
        case 14: return js_num(x < 0 ? -kpow(-x, 1.0 / 3) : kpow(x, 1.0 / 3));
        case 15: return js_num((double)(int)((u32)to_i32(x) * (u32)to_i32(y)));
        case 16: { u32 v = to_u32(x); int n = 0; if(!v) return js_num(32); while(!(v & 0x80000000u)){ v <<= 1; n++; } return js_num(n); }
        case 17: { float f = (float)x; return js_num((double)f); }
        case 18: return js_num(klog(1 + x));
        case 19: return js_num(kexp(x) - 1);
        case 20: return js_num((kexp(x) - kexp(-x)) / 2);
        case 21: return js_num((kexp(x) + kexp(-x)) / 2);
        case 22: { double a = kexp(x), b = kexp(-x); return js_num((a - b) / (a + b)); }
    }
    return js_num(nan_value());
}

/* ---- globals ---- */
static int uri_keep(unsigned char c, const char *extra){
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    if(strchr("-_.!~*'()", c)) return 1;
    return extra && c && strchr(extra, c) != NULL;
}
NATIVE(g_encode_uri){
    (void)self;
    const char *extra = ctx ? ";,/?:@&=+$#" : NULL;
    const unsigned char *s = (const unsigned char *)(argc ? js_to_str(argv[0]) : "undefined");
    sb_t o; sb_init(&o, 64);
    for(; *s; s++){
        if(uri_keep(*s, extra)) sb_putc(&o, (char)*s);
        else { char b[4] = { '%', "0123456789ABCDEF"[*s >> 4], "0123456789ABCDEF"[*s & 15], 0 }; sb_putn(&o, b, 3); }
    }
    return js_str(o.b);
}
NATIVE(g_decode_uri){
    (void)self;
    const char *s = argc ? js_to_str(argv[0]) : "undefined";
    sb_t o; sb_init(&o, 64);
    for(; *s; s++){
        if(*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0){
            char c = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            if(ctx && strchr(";,/?:@&=+$#", c)) sb_putn(&o, s, 3);
            else sb_putc(&o, c);
            s += 2;
        } else if(*s == '%' && !ctx){ return throw_val(make_error(6, "URI malformed")); }
        else sb_putc(&o, *s);
    }
    return js_str(o.b);
}
NATIVE(g_escape){
    (void)self; (void)ctx;
    const unsigned char *s = (const unsigned char *)(argc ? js_to_str(argv[0]) : "undefined");
    sb_t o; sb_init(&o, 64);
    for(; *s; s++){
        if((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("@*_+-./", *s)) sb_putc(&o, (char)*s);
        else { char b[4] = { '%', "0123456789ABCDEF"[*s >> 4], "0123456789ABCDEF"[*s & 15], 0 }; sb_putn(&o, b, 3); }
    }
    return js_str(o.b);
}
static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
NATIVE(g_btoa){
    (void)self; (void)ctx;
    const unsigned char *s = (const unsigned char *)(argc ? js_to_str(argv[0]) : "undefined");
    /* the input is a "binary string": decode UTF-8 back to code units 0..255 */
    u32 L = (u32)strlen((const char *)s);
    unsigned char *bytes = jalloc(L + 1); u32 n = 0;
    for(u32 i = 0; i < L; ){
        u32 c = s[i];
        if(c >= 0xC0 && c < 0xE0 && i + 1 < L){ c = ((c & 31) << 6) | (s[i+1] & 63); i += 2; }
        else if(c >= 0xE0){ return throw_val(make_error(0, "The string to be encoded contains characters outside of the Latin1 range.")); }
        else i++;
        bytes[n++] = (unsigned char)c;
    }
    sb_t o; sb_init(&o, n * 4 / 3 + 8);
    for(u32 i = 0; i < n; i += 3){
        u32 v = (u32)bytes[i] << 16 | (i + 1 < n ? (u32)bytes[i+1] << 8 : 0) | (i + 2 < n ? bytes[i+2] : 0);
        char q[4] = { B64[(v >> 18) & 63], B64[(v >> 12) & 63], i + 1 < n ? B64[(v >> 6) & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
        sb_putn(&o, q, 4);
    }
    return js_str(o.b);
}
NATIVE(g_atob){
    (void)self; (void)ctx;
    const char *s = argc ? js_to_str(argv[0]) : "";
    sb_t o; sb_init(&o, (u32)strlen(s));
    u32 acc = 0; int bits = 0;
    for(; *s; s++){
        const char *p = strchr(B64, *s);
        if(*s == '=' || *s == ' ' || *s == '\n') continue;
        if(!p){ if(*s == '-') p = B64 + 62; else if(*s == '_') p = B64 + 63; else continue; }
        acc = (acc << 6) | (u32)(p - B64); bits += 6;
        if(bits >= 8){
            bits -= 8;
            u32 c = (acc >> bits) & 255;
            char b[4]; u32 k = put_utf8(b, c);   /* each byte becomes one code unit */
            sb_putn(&o, b, k);
        }
    }
    return js_str(o.b);
}

/* ---- Date (UTC; the host sets the wall clock with js_set_time) ---- */
static double clock_base_ms = 1767225600000.0;   /* 2026-01-01, until the host says otherwise */
static u64 clock_base_ticks = 0;
void js_set_time(double epoch_ms){ clock_base_ms = epoch_ms; clock_base_ticks = timer_ticks(); }
static double now_ms(void){ return clock_base_ms + (double)(timer_ticks() - clock_base_ticks) * 10.0; }
static js_obj_t *date_proto = NULL;
static double days_from_civil(double y, double m, double d){   /* m 1..12 */
    y -= m <= 2;
    double era = kfloor(y / 400);
    double yoe = y - era * 400;
    double mp = m > 2 ? m - 3 : m + 9;
    double doy = kfloor((153 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + kfloor(yoe / 4) - kfloor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(double z, int *yy, int *mm, int *dd){
    z += 719468;
    double era = kfloor(z / 146097);
    double doe = z - era * 146097;
    double yoe = kfloor((doe - kfloor(doe / 1460) + kfloor(doe / 36524) - kfloor(doe / 146096)) / 365);
    double y = yoe + era * 400;
    double doy = doe - (365 * yoe + kfloor(yoe / 4) - kfloor(yoe / 100));
    double mp = kfloor((5 * doy + 2) / 153);
    double d = doy - kfloor((153 * mp + 2) / 5) + 1;
    double m = mp < 10 ? mp + 3 : mp - 9;
    *yy = (int)(y + (m <= 2)); *mm = (int)m; *dd = (int)d;
}
static double make_time(double y, double mon0, double d, double h, double mi, double s, double ms){
    double ym = y + kfloor(mon0 / 12), mn = kfmod(mon0, 12); if(mn < 0) mn += 12;
    return (days_from_civil(ym, mn + 1, 1) + d - 1) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}
static double parse_date(const char *s){
    int v[7] = { 0, 1, 1, 0, 0, 0, 0 }; int k = 0;
    const char *p = s;
    while(*p == ' ') p++;
    int sign = 1; if(*p == '+' || *p == '-'){ if(*p == '-') sign = -1; p++; }
    for(; *p && k < 7; ){
        if(*p >= '0' && *p <= '9'){
            int n = 0, digits = 0;
            while(*p >= '0' && *p <= '9'){ if(digits < 9) n = n * 10 + (*p - '0'); digits++; p++; }
            if(k == 6){ while(digits < 3){ n *= 10; digits++; } while(digits > 3){ n /= 10; digits--; } }
            v[k++] = n;
            if(*p == '-' || *p == ':' || *p == 'T' || *p == '.' || *p == ' ') { p++; continue; }
            break;
        } else return nan_value();
    }
    if(k < 1) return nan_value();
    double t = make_time(sign * v[0], v[1] - 1, v[2], v[3], v[4], v[5], v[6]);
    if(*p == '+' || *p == '-'){
        int sg = *p == '-' ? -1 : 1; p++;
        int hh = 0, mm = 0;
        if(p[0] && p[1]){ hh = (p[0]-'0') * 10 + (p[1]-'0'); p += 2; }
        if(*p == ':') p++;
        if(p[0] && p[1]) mm = (p[0]-'0') * 10 + (p[1]-'0');
        t -= sg * (hh * 3600000.0 + mm * 60000.0);
    }
    return t;
}
static double *date_slot(js_val_t self){ return self.t == JS_OBJ && self.o->kind == K_DATE ? (double *)self.o->slot : NULL; }
static const char *date_iso(double t){
    if(t != t) return NULL;
    double days = kfloor(t / 86400000.0), ms = t - days * 86400000.0;
    int y, m, d; civil_from_days(days, &y, &m, &d);
    int h = (int)(ms / 3600000), mi = (int)(kfmod(ms, 3600000) / 60000), s = (int)(kfmod(ms, 60000) / 1000), mss = (int)kfmod(ms, 1000);
    char b[32];
    int yy = y < 0 ? -y : y;
    b[0] = (char)('0' + yy / 1000 % 10); b[1] = (char)('0' + yy / 100 % 10); b[2] = (char)('0' + yy / 10 % 10); b[3] = (char)('0' + yy % 10);
    b[4] = '-'; b[5] = (char)('0' + m / 10); b[6] = (char)('0' + m % 10); b[7] = '-'; b[8] = (char)('0' + d / 10); b[9] = (char)('0' + d % 10);
    b[10] = 'T'; b[11] = (char)('0' + h / 10); b[12] = (char)('0' + h % 10); b[13] = ':'; b[14] = (char)('0' + mi / 10); b[15] = (char)('0' + mi % 10);
    b[16] = ':'; b[17] = (char)('0' + s / 10); b[18] = (char)('0' + s % 10); b[19] = '.';
    b[20] = (char)('0' + mss / 100); b[21] = (char)('0' + mss / 10 % 10); b[22] = (char)('0' + mss % 10); b[23] = 'Z'; b[24] = 0;
    return jstrdup(b, 24);
}
NATIVE(date_ctor){
    (void)ctx;
    js_obj_t *o = ctor_self(self, date_proto);
    double t;
    if(argc == 0) t = now_ms();
    else if(argc == 1){
        js_val_t v = argv[0];
        double *ds = date_slot(v);
        if(ds) t = *ds;
        else if(v.t == JS_STR) t = parse_date(v.s);
        else t = js_to_num(v);
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for(int i = 0; i < argc && i < 7; i++) f[i] = js_to_num(argv[i]);
        if(f[0] >= 0 && f[0] <= 99) f[0] += 1900;
        t = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    if(t == t) t = ktrunc(t);
    if(!o){
        /* Date() called as a function returns a string */
        const char *s = date_iso(now_ms());
        return js_str(s ? s : "Invalid Date");
    }
    double *slot = jalloc(sizeof(double)); *slot = t;
    o->kind = K_DATE; o->slot = slot;
    return js_objv(o);
}
NATIVE(date_now2){ (void)self; (void)ctx; (void)argc; (void)argv; return js_num(kfloor(now_ms())); }
NATIVE(date_parse){ (void)self; (void)ctx; return js_num(argc ? parse_date(js_to_str(argv[0])) : nan_value()); }
NATIVE(date_utc){
    (void)self; (void)ctx;
    double f[7] = { nan_value(), 0, 1, 0, 0, 0, 0 };
    for(int i = 0; i < argc && i < 7; i++) f[i] = js_to_num(argv[i]);
    if(f[0] >= 0 && f[0] <= 99) f[0] += 1900;
    return js_num(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
}
NATIVE(date_get){
    double *ds = date_slot(self);
    if(!ds) return throw_type("this is not a Date object.");
    double t = *ds;
    int what = (int)(uintptr_t)ctx;
    if(what == 0) return js_num(t);
    if(t != t) return js_num(nan_value());
    double days = kfloor(t / 86400000.0), ms = t - days * 86400000.0;
    int y, m, d; civil_from_days(days, &y, &m, &d);
    switch(what){
        case 1: return js_num(y);
        case 2: return js_num(m - 1);
        case 3: return js_num(d);
        case 4: { double wd = kfmod(days + 4, 7); if(wd < 0) wd += 7; return js_num(wd); }
        case 5: return js_num(kfloor(ms / 3600000));
        case 6: return js_num(kfloor(kfmod(ms, 3600000) / 60000));
        case 7: return js_num(kfloor(kfmod(ms, 60000) / 1000));
        case 8: return js_num(kfmod(ms, 1000));
        case 9: return js_num(0);                          /* getTimezoneOffset: UTC */
    }
    (void)argc; (void)argv;
    return js_undef();
}
NATIVE(date_set){
    double *ds = date_slot(self);
    if(!ds) return throw_type("this is not a Date object.");
    int what = (int)(uintptr_t)ctx;
    double t = *ds;
    if(what == 0){ *ds = argn(argc, argv, 0); return js_num(*ds); }
    if(t != t) t = 0;
    double days = kfloor(t / 86400000.0), ms = t - days * 86400000.0;
    int y, m, d; civil_from_days(days, &y, &m, &d);
    double f[7] = { y, m - 1, d, kfloor(ms / 3600000), kfloor(kfmod(ms, 3600000) / 60000), kfloor(kfmod(ms, 60000) / 1000), kfmod(ms, 1000) };
    /* what: 1 year, 2 month, 3 date, 5 hours, 6 minutes, 7 seconds, 8 ms; later args set the following fields */
    int base = what == 1 ? 0 : what == 2 ? 1 : what == 3 ? 2 : what - 2;
    for(int i = 0; i < argc && base + i < 7; i++) f[base + i] = js_to_num(argv[i]);
    *ds = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    return js_num(*ds);
}
NATIVE(date_iso2){
    (void)ctx; (void)argc; (void)argv;
    double *ds = date_slot(self);
    if(!ds) return throw_type("this is not a Date object.");
    const char *s = date_iso(*ds);
    if(!s) return (int)(uintptr_t)ctx == 1 ? js_null() : throw_val(make_error(2, "Invalid time value"));
    return js_str(s);
}
NATIVE(date_to_string){
    (void)ctx; (void)argc; (void)argv;
    double *ds = date_slot(self);
    if(!ds || *ds != *ds) return js_str("Invalid Date");
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    double t = *ds, days = kfloor(t / 86400000.0);
    int y, m, d; civil_from_days(days, &y, &m, &d);
    int w = (int)kfmod(kfmod(days + 4, 7) + 7, 7);
    const char *iso = date_iso(t);
    sb_t s; sb_init(&s, 48);
    sb_puts(&s, wd[w]); sb_putc(&s, ' '); sb_puts(&s, mn[m - 1]); sb_putc(&s, ' ');
    sb_putn(&s, iso + 8, 2); sb_putc(&s, ' '); sb_putn(&s, iso, 4); sb_putc(&s, ' ');
    sb_putn(&s, iso + 11, 8); sb_puts(&s, " GMT+0000 (Coordinated Universal Time)");
    return js_str(s.b);
}

/* ---- Promise and the job queue ---- */
typedef struct { int state; js_val_t value; js_obj_t *reactions; int handled; } promise_t;   /* 0 pending 1 fulfilled 2 rejected */
static js_obj_t *promise_proto = NULL;
static js_obj_t *job_queue = NULL;   /* pairs: function, argument */
static int job_head = 0;
static void enqueue_job(js_val_t fn, js_val_t arg){
    if(!job_queue) return;
    arr_push(job_queue, fn); arr_push(job_queue, arg);
}
/* Runs queued jobs (promise reactions, queueMicrotask) until none are left. */
int js_drain_jobs(void){
    int ran = 0;
    if(!job_queue || depth > 0) return 0;
    while(job_head < job_queue->nitems && ran < 1000000){
        js_val_t fn = job_queue->items[job_head], a = job_queue->items[job_head + 1];
        job_head += 2;
        if(job_head >= job_queue->nitems){ job_queue->nitems = 0; job_head = 0; }
        int sf = failed, st = thrown;
        call_value(fn, js_undef(), 1, &a);
        js_val_t ex;
        if(!sf && !st && take_exception(&ex)){ /* an uncaught error in a job: dropped, like a browser */ }
        ran++;
    }
    return ran;
}
static js_obj_t *new_promise(void){
    js_obj_t *p = new_with_proto(promise_proto);
    promise_t *ps = jalloc(sizeof(promise_t));
    ps->state = 0; ps->value = js_undef(); ps->reactions = new_array(); ps->handled = 0;
    p->kind = K_PROMISE; p->slot = ps;
    return p;
}
static promise_t *pslot(js_val_t v){ return v.t == JS_OBJ && v.o->kind == K_PROMISE ? (promise_t *)v.o->slot : NULL; }
/* a reaction record: [onFulfilled, onRejected, child promise] */
NATIVE(reaction_job);
static void settle(js_obj_t *p, int state, js_val_t v){
    promise_t *ps = p->slot;
    if(ps->state) return;
    ps->state = state; ps->value = v;
    for(int i = 0; i < ps->reactions->nitems; i++){
        js_obj_t *job = new_array();
        arr_push(job, ps->reactions->items[i]); arr_push(job, js_objv(p));
        enqueue_job(js_objv(js_new_native(reaction_job, NULL)), js_objv(job));
    }
    ps->reactions->nitems = 0;
}
static void resolve_promise(js_obj_t *p, js_val_t x);
typedef struct { js_obj_t *p; int *done; } resolver_t;
NATIVE(resolve_fn){ (void)self; resolver_t *r = ctx; if(*r->done) return js_undef(); *r->done = 1; resolve_promise(r->p, argc ? argv[0] : js_undef()); return js_undef(); }
NATIVE(reject_fn){ (void)self; resolver_t *r = ctx; if(*r->done) return js_undef(); *r->done = 1; settle(r->p, 2, argc ? argv[0] : js_undef()); return js_undef(); }
static void make_resolvers(js_obj_t *p, js_val_t *res, js_val_t *rej){
    int *done = jalloc(sizeof(int)); *done = 0;
    resolver_t *a = jalloc(sizeof(resolver_t)); a->p = p; a->done = done;
    resolver_t *b = jalloc(sizeof(resolver_t)); b->p = p; b->done = done;
    *res = js_objv(js_new_native(resolve_fn, a));
    *rej = js_objv(js_new_native(reject_fn, b));
}
NATIVE(thenable_job){
    (void)self; (void)ctx; (void)argc;
    /* argv[0] = [promise, thenable, then] */
    js_obj_t *job = argv[0].o;
    js_obj_t *p = job->items[0].o;
    js_val_t res, rej; make_resolvers(p, &res, &rej);
    js_val_t a[2] = { res, rej };
    call_value(job->items[2], job->items[1], 2, a);
    js_val_t ex;
    if(take_exception(&ex)) call_value(rej, js_undef(), 1, &ex);
    return js_undef();
}
static void resolve_promise(js_obj_t *p, js_val_t x){
    if(x.t == JS_OBJ && x.o == p){ settle(p, 2, make_error(1, "Chaining cycle detected for promise")); return; }
    if(x.t == JS_OBJ){
        js_val_t then = js_get(x.o, "then");
        js_val_t ex;
        if(take_exception(&ex)){ settle(p, 2, ex); return; }
        if(js_callable(then)){
            js_obj_t *job = new_array();
            arr_push(job, js_objv(p)); arr_push(job, x); arr_push(job, then);
            enqueue_job(js_objv(js_new_native(thenable_job, NULL)), js_objv(job));
            return;
        }
    }
    settle(p, 1, x);
}
NATIVE(reaction_job){
    (void)self; (void)ctx; (void)argc;
    js_obj_t *job = argv[0].o;
    js_obj_t *r = job->items[0].o;              /* [onF, onR, child] */
    promise_t *src = job->items[1].o->slot;
    js_val_t h = src->state == 1 ? r->items[0] : r->items[1];
    js_obj_t *child = r->items[2].t == JS_OBJ ? r->items[2].o : NULL;
    if(!js_callable(h)){
        if(child) { if(src->state == 1) resolve_promise(child, src->value); else settle(child, 2, src->value); }
        return js_undef();
    }
    js_val_t v = call_value(h, js_undef(), 1, &src->value);
    js_val_t ex;
    if(take_exception(&ex)){ if(child) settle(child, 2, ex); }
    else if(child) resolve_promise(child, v);
    return js_undef();
}
static js_obj_t *promise_then(js_obj_t *p, js_val_t onf, js_val_t onr){
    promise_t *ps = p->slot;
    js_obj_t *child = new_promise();
    js_obj_t *r = new_array();
    arr_push(r, onf); arr_push(r, onr); arr_push(r, js_objv(child));
    ps->handled = 1;
    if(ps->state == 0) arr_push(ps->reactions, js_objv(r));
    else {
        js_obj_t *job = new_array();
        arr_push(job, js_objv(r)); arr_push(job, js_objv(p));
        enqueue_job(js_objv(js_new_native(reaction_job, NULL)), js_objv(job));
    }
    return child;
}
NATIVE(promise_ctor){
    (void)ctx;
    if(!ctor_self(self, promise_proto)) return throw_type("Promise constructor cannot be invoked without 'new'");
    if(argc < 1 || !js_callable(argv[0])) return throw_type("Promise resolver is not a function");
    js_obj_t *p = self.o;
    promise_t *ps = jalloc(sizeof(promise_t));
    ps->state = 0; ps->value = js_undef(); ps->reactions = new_array(); ps->handled = 0;
    p->kind = K_PROMISE; p->slot = ps;
    js_val_t res, rej; make_resolvers(p, &res, &rej);
    js_val_t a[2] = { res, rej };
    call_value(argv[0], js_undef(), 2, a);
    js_val_t ex;
    if(take_exception(&ex)) call_value(rej, js_undef(), 1, &ex);
    return self;
}
NATIVE(promise_then_m){
    (void)ctx;
    if(!pslot(self)) return throw_type("Method Promise.prototype.then called on incompatible receiver");
    return js_objv(promise_then(self.o, argc > 0 ? argv[0] : js_undef(), argc > 1 ? argv[1] : js_undef()));
}
NATIVE(promise_catch_m){
    (void)ctx;
    if(!pslot(self)){ js_val_t then = self.t == JS_OBJ ? js_get(self.o, "then") : js_undef(); js_val_t a[2] = { js_undef(), argc ? argv[0] : js_undef() }; return js_callable(then) ? call_value(then, self, 2, a) : js_undef(); }
    return js_objv(promise_then(self.o, js_undef(), argc ? argv[0] : js_undef()));
}
NATIVE(finally_pass){
    (void)self;
    js_obj_t *f = ctx;   /* [onFinally, isReject] */
    call_value(f->items[0], js_undef(), 0, NULL);
    if(STOP()) return js_undef();
    if(js_truthy(f->items[1])) return throw_val(argc ? argv[0] : js_undef());
    return argc ? argv[0] : js_undef();
}
NATIVE(promise_finally_m){
    (void)ctx;
    if(!pslot(self)) return self;
    js_val_t fin = argc ? argv[0] : js_undef();
    if(!js_callable(fin)) return js_objv(promise_then(self.o, fin, fin));
    js_obj_t *a = new_array(); arr_push(a, fin); arr_push(a, js_bool(0));
    js_obj_t *b = new_array(); arr_push(b, fin); arr_push(b, js_bool(1));
    return js_objv(promise_then(self.o, js_objv(js_new_native(finally_pass, a)), js_objv(js_new_native(finally_pass, b))));
}
static js_val_t promise_resolve_val(js_val_t v){
    if(pslot(v)) return v;
    js_obj_t *p = new_promise();
    resolve_promise(p, v);
    return js_objv(p);
}
NATIVE(promise_resolve_s){ (void)self; (void)ctx; return promise_resolve_val(argc ? argv[0] : js_undef()); }
NATIVE(promise_reject_s){ (void)self; (void)ctx; js_obj_t *p = new_promise(); settle(p, 2, argc ? argv[0] : js_undef()); return js_objv(p); }
/* Promise.all / allSettled / race / any: ctx = mode */
typedef struct { js_obj_t *result, *values; int *left; int index, mode; } combo_t;
NATIVE(combo_cb){
    (void)self;
    combo_t *c = ctx;
    js_val_t v = argc ? argv[0] : js_undef();
    int rejected = c->mode >= 10;              /* +10: the rejection handler */
    int mode = c->mode % 10;
    promise_t *rs = c->result->slot;
    if(rs->state) return js_undef();
    if(mode == 0){                             /* all */
        if(rejected){ settle(c->result, 2, v); return js_undef(); }
        c->values->items[c->index] = v;
    } else if(mode == 1){                      /* allSettled */
        js_obj_t *o = js_new_object();
        js_set(o, "status", js_str(rejected ? "rejected" : "fulfilled"));
        js_set(o, rejected ? "reason" : "value", v);
        c->values->items[c->index] = js_objv(o);
    } else if(mode == 2){                      /* race */
        if(rejected) settle(c->result, 2, v); else resolve_promise(c->result, v);
        return js_undef();
    } else {                                   /* any */
        if(!rejected){ resolve_promise(c->result, v); return js_undef(); }
        c->values->items[c->index] = v;
    }
    if(--*c->left == 0){
        if(mode == 3){ js_val_t e = make_error(7, "All promises were rejected"); js_set(e.o, "errors", js_objv(c->values)); settle(c->result, 2, e); }
        else resolve_promise(c->result, js_objv(c->values));
    }
    return js_undef();
}
NATIVE(promise_combo){
    (void)self;
    int mode = (int)(uintptr_t)ctx;
    js_obj_t *result = new_promise();
    js_obj_t *items = iterate_to_array(argc ? argv[0] : js_undef());
    js_obj_t *values = new_array();
    int *left = jalloc(sizeof(int)); *left = items->nitems;
    for(int i = 0; i < items->nitems; i++) arr_push(values, js_undef());
    if(items->nitems == 0){
        if(mode == 3) settle(result, 2, make_error(7, "All promises were rejected"));
        else if(mode != 2) resolve_promise(result, js_objv(values));
        return js_objv(result);
    }
    for(int i = 0; i < items->nitems; i++){
        js_val_t p = promise_resolve_val(items->items[i]);
        combo_t *ok = jalloc(sizeof(combo_t)), *bad = jalloc(sizeof(combo_t));
        ok->result = bad->result = result; ok->values = bad->values = values; ok->left = bad->left = left;
        ok->index = bad->index = i; ok->mode = mode; bad->mode = mode + 10;
        if(pslot(p)) promise_then(p.o, js_objv(js_new_native(combo_cb, ok)), js_objv(js_new_native(combo_cb, bad)));
    }
    return js_objv(result);
}
NATIVE(g_queue_microtask){
    (void)self; (void)ctx;
    if(argc < 1 || !js_callable(argv[0])) return throw_type("queueMicrotask needs a function");
    enqueue_job(argv[0], js_undef());
    return js_undef();
}

/* ---- Map, Set, WeakMap, WeakSet: insertion-ordered entries plus a hash index ---- */
typedef struct { js_obj_t *keys, *vals; u32 *index; u32 cap; int live; } map_t;
static js_obj_t *map_proto = NULL, *set_proto = NULL, *weakmap_proto = NULL, *weakset_proto = NULL;
static int is_dead(js_val_t k){ return k.t == JS_UNDEF && k.b == 0x7fff; }
static u32 key_hash(js_val_t k){
    u32 h = 2166136261u;
    switch(k.t){
        case JS_STR: for(const char *p = k.s; *p; p++) h = (h ^ (u8)*p) * 16777619u; return h;
        case JS_NUM: { double d = k.n; if(d == 0) d = 0; if(d != d) return 7; u64 b; memcpy(&b, &d, 8); return (u32)(b ^ (b >> 32)) * 2654435761u; }
        case JS_OBJ: { uintptr_t p = (uintptr_t)k.o; return (u32)((p >> 4) ^ (p >> 20)) * 2654435761u; }
        case JS_BOOL: return k.b ? 3 : 5;
        case JS_NULL: return 11;
    }
    return 13;
}
static int key_eq(js_val_t a, js_val_t b){
    if(a.t != b.t) return 0;
    switch(a.t){
        case JS_STR: return strcmp(a.s, b.s) == 0;
        case JS_NUM: return a.n == b.n || (a.n != a.n && b.n != b.n);
        case JS_OBJ: return a.o == b.o;
        case JS_BOOL: return a.b == b.b;
    }
    return 1;
}
static map_t *new_map_data(void){
    map_t *m = jalloc(sizeof(map_t));
    m->keys = new_array(); m->vals = new_array(); m->cap = 0; m->index = NULL; m->live = 0;
    return m;
}
static void map_reindex(map_t *m){
    u32 cap = 16; while(cap < (u32)m->keys->nitems * 2 + 8) cap *= 2;
    m->index = jalloc(sizeof(u32) * cap); memset(m->index, 0, sizeof(u32) * cap); m->cap = cap;
    for(int i = 0; i < m->keys->nitems; i++){
        if(is_dead(m->keys->items[i])) continue;
        u32 j = key_hash(m->keys->items[i]) & (cap - 1);
        while(m->index[j]) j = (j + 1) & (cap - 1);
        m->index[j] = (u32)i + 1;
    }
}
static int map_find(map_t *m, js_val_t k){
    if(!m->cap) return -1;
    u32 j = key_hash(k) & (m->cap - 1);
    while(m->index[j]){
        int i = (int)m->index[j] - 1;
        if(!is_dead(m->keys->items[i]) && key_eq(m->keys->items[i], k)) return i;
        j = (j + 1) & (m->cap - 1);
    }
    return -1;
}
static void map_put(map_t *m, js_val_t k, js_val_t v){
    if(k.t == JS_NUM && k.n == 0) k.n = 0;        /* -0 is stored as +0 */
    int i = map_find(m, k);
    if(i >= 0){ m->vals->items[i] = v; return; }
    if((u32)(m->keys->nitems + 1) * 2 > m->cap){
        if(m->live * 2 < m->keys->nitems){        /* mostly deleted: compact */
            js_obj_t *nk = new_array(), *nv = new_array();
            for(int x = 0; x < m->keys->nitems; x++) if(!is_dead(m->keys->items[x])){ arr_push(nk, m->keys->items[x]); arr_push(nv, m->vals->items[x]); }
            m->keys = nk; m->vals = nv;
        }
        arr_push(m->keys, k); arr_push(m->vals, v); m->live++;
        map_reindex(m);
        return;
    }
    arr_push(m->keys, k); arr_push(m->vals, v); m->live++;
    u32 j = key_hash(k) & (m->cap - 1);
    while(m->index[j]) j = (j + 1) & (m->cap - 1);
    m->index[j] = (u32)m->keys->nitems;
}
static int map_del(map_t *m, js_val_t k){
    int i = map_find(m, k);
    if(i < 0) return 0;
    js_val_t dead = js_undef(); dead.b = 0x7fff;
    m->keys->items[i] = dead; m->vals->items[i] = js_undef(); m->live--;
    return 1;
}
static map_t *mdata(js_val_t self, int kind){
    if(self.t != JS_OBJ || self.o->kind != kind) return NULL;
    return self.o->slot;
}
/* what: 0 keys, 1 values, 2 [key, value] entries */
static js_obj_t *map_items(js_obj_t *o, int what){
    map_t *m = o->slot;
    js_obj_t *out = new_array();
    if(!m) return out;
    for(int i = 0; i < m->keys->nitems; i++){
        if(is_dead(m->keys->items[i])) continue;
        if(what == 0) arr_push(out, m->keys->items[i]);
        else if(what == 1) arr_push(out, m->vals->items[i]);
        else { js_obj_t *e = new_array(); arr_push(e, m->keys->items[i]); arr_push(e, m->vals->items[i]); arr_push(out, js_objv(e)); }
    }
    return out;
}
NATIVE(mapset_ctor){
    int which = (int)(uintptr_t)ctx;                 /* 0 Map, 1 Set, 2 WeakMap, 3 WeakSet */
    js_obj_t *proto = which == 0 ? map_proto : which == 1 ? set_proto : which == 2 ? weakmap_proto : weakset_proto;
    js_obj_t *o = ctor_self(self, proto);
    if(!o) return throw_type("Constructor requires 'new'");
    o->kind = (which == 0 || which == 2) ? K_MAP : K_SET;
    o->slot = new_map_data();
    if(argc > 0 && argv[0].t != JS_UNDEF && argv[0].t != JS_NULL){
        js_obj_t *items = iterate_to_array(argv[0]);
        for(int i = 0; i < items->nitems && !STOP(); i++){
            js_val_t it = items->items[i];
            if(o->kind == K_MAP){
                if(it.t != JS_OBJ) return throw_type("Iterator value is not an entry object");
                js_val_t k = it.o->is_array ? (it.o->nitems > 0 ? it.o->items[0] : js_undef()) : js_get(it.o, "0");
                js_val_t v = it.o->is_array ? (it.o->nitems > 1 ? it.o->items[1] : js_undef()) : js_get(it.o, "1");
                map_put(o->slot, k, v);
            } else map_put(o->slot, it, it);
        }
    }
    return js_objv(o);
}
NATIVE(map_m){
    int op = (int)(uintptr_t)ctx;
    int want = op >= 100 ? K_SET : K_MAP; op %= 100;
    map_t *m = mdata(self, want);
    if(!m) return throw_type(want == K_MAP ? "Method Map.prototype called on incompatible receiver" : "Method Set.prototype called on incompatible receiver");
    js_val_t k = argc > 0 ? argv[0] : js_undef();
    switch(op){
        case 0: { int i = map_find(m, k); return i >= 0 ? m->vals->items[i] : js_undef(); }       /* get */
        case 1: map_put(m, k, want == K_SET ? k : (argc > 1 ? argv[1] : js_undef())); return self;  /* set / add */
        case 2: return js_bool(map_find(m, k) >= 0);                                               /* has */
        case 3: return js_bool(map_del(m, k));                                                     /* delete */
        case 4: m->keys->nitems = 0; m->vals->nitems = 0; m->live = 0; m->cap = 0; m->index = NULL; return js_undef();
        case 5: return js_num(m->live);                                                            /* size */
        case 6: {                                                                                  /* forEach */
            if(!js_callable(k)) return throw_type("forEach callback is not a function");
            for(int i = 0; i < m->keys->nitems && !STOP(); i++){
                if(is_dead(m->keys->items[i])) continue;
                js_val_t a[3] = { m->vals->items[i], m->keys->items[i], self };
                call_value(k, argc > 1 ? argv[1] : js_undef(), 3, a);
            }
            return js_undef();
        }
        case 7: return make_array_iter(map_items(self.o, 0));
        case 8: return make_array_iter(map_items(self.o, want == K_SET ? 0 : 1));
        case 9: {
            if(want == K_MAP) return make_array_iter(map_items(self.o, 2));
            js_obj_t *ks = map_items(self.o, 0), *out = new_array();
            for(int i = 0; i < ks->nitems; i++){ js_obj_t *e = new_array(); arr_push(e, ks->items[i]); arr_push(e, ks->items[i]); arr_push(out, js_objv(e)); }
            return make_array_iter(out);
        }
    }
    return js_undef();
}

/* ---- typed arrays: arrays whose writes are converted to the element type ---- */
enum { TA_I8, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64 };
static const char *ta_names[] = { "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array" };
static const int ta_size[] = { 1, 1, 1, 2, 2, 4, 4, 4, 8 };
static js_obj_t *ta_protos[9];
static js_obj_t *buffer_proto = NULL;
static double ta_conv(int type, double d){
    switch(type){
        case TA_I8:  return (double)(signed char)(to_u32(d) & 255);
        case TA_U8:  return (double)(to_u32(d) & 255);
        case TA_U8C: { if(d != d || d <= 0) return 0; if(d >= 255) return 255; double f = kfloor(d), r = d - f; if(r > 0.5 || (r == 0.5 && ((long long)f & 1))) f += 1; return f; }
        case TA_I16: return (double)(short)(to_u32(d) & 65535);
        case TA_U16: return (double)(to_u32(d) & 65535);
        case TA_I32: return (double)to_i32(d);
        case TA_U32: return (double)to_u32(d);
        case TA_F32: return (double)(float)d;
    }
    return d;
}
static void typed_set(js_obj_t *o, const char *name, js_val_t v){
    int i = as_index(name);
    if(i < 0){ js_set(o, name, v); return; }
    if(i >= o->nitems) return;
    o->items[i] = js_num(ta_conv(*(int *)o->slot, js_to_num(v)));
}
static js_obj_t *make_typed(int type, int n){
    js_obj_t *a = new_obj();
    a->is_array = 1; a->kind = K_TYPED; a->proto = ta_protos[type];
    int *t = jalloc(sizeof(int)); *t = type; a->slot = t;
    for(int i = 0; i < n; i++) arr_push(a, js_num(0));
    return a;
}
NATIVE(typed_ctor){
    int type = (int)(uintptr_t)ctx;
    (void)self;
    js_obj_t *a;
    if(argc == 0 || argv[0].t != JS_OBJ){
        double n = argc ? js_to_num(argv[0]) : 0;
        if(n != n) n = 0;
        if(n < 0 || n > 64e6) return throw_val(make_error(2, "Invalid typed array length"));
        a = make_typed(type, (int)n);
    } else if(argv[0].o->kind == K_BUFFER){
        js_obj_t *bytes = argv[0].o->slot;   /* the buffer's bytes, as numbers */
        int off = argi(argc, argv, 1, 0);
        int es = ta_size[type];
        int n = argc > 2 && argv[2].t != JS_UNDEF ? argi(argc, argv, 2, 0) : (bytes->nitems - off) / es;
        a = make_typed(type, n < 0 ? 0 : n);
        for(int i = 0; i < a->nitems; i++){
            u64 raw = 0;
            for(int b = 0; b < es; b++){ int bi = off + i * es + b; if(bi < bytes->nitems) raw |= (u64)((u32)js_to_num(bytes->items[bi]) & 255) << (8 * b); }
            double d;
            if(type == TA_F32){ u32 r32 = (u32)raw; float f; memcpy(&f, &r32, 4); d = f; }
            else if(type == TA_F64){ memcpy(&d, &raw, 8); }
            else d = ta_conv(type, (double)raw);
            a->items[i] = js_num(d);
        }
        js_set(a, "buffer", argv[0]); find_own(a, "buffer")->flags = PF_HIDDEN;
        return js_objv(a);
    } else {
        js_obj_t *src = iterate_to_array(argv[0]);
        if(!argv[0].o->is_array && argv[0].o->kind != K_TYPED && src->nitems == 0){
            double L = js_to_num(js_get(argv[0].o, "length"));
            if(L == L) for(int i = 0; i < (int)L; i++) arr_push(src, js_get(argv[0].o, idx_str(i)));
        }
        a = make_typed(type, src->nitems);
        for(int i = 0; i < src->nitems; i++) a->items[i] = js_num(ta_conv(type, js_to_num(src->items[i])));
    }
    return js_objv(a);
}
NATIVE(typed_length){ (void)ctx; (void)argc; (void)argv; return js_num(self.t == JS_OBJ ? self.o->nitems : 0); }
NATIVE(typed_byte_length){ (void)ctx; (void)argc; (void)argv; if(self.t != JS_OBJ || self.o->kind != K_TYPED) return js_num(0); return js_num(self.o->nitems * ta_size[*(int *)self.o->slot]); }
NATIVE(typed_subarray){
    (void)ctx;
    if(self.t != JS_OBJ || self.o->kind != K_TYPED) return js_undef();
    int n = self.o->nitems, s = argi(argc, argv, 0, 0), e = argi(argc, argv, 1, n);
    if(s < 0) s += n; if(e < 0) e += n; if(s < 0) s = 0; if(e > n) e = n; if(e < s) e = s;
    js_obj_t *a = make_typed(*(int *)self.o->slot, e - s);
    for(int i = s; i < e; i++) a->items[i - s] = self.o->items[i];
    return js_objv(a);
}
NATIVE(typed_set_m){
    (void)ctx;
    if(self.t != JS_OBJ || self.o->kind != K_TYPED || argc < 1) return js_undef();
    js_obj_t *src = iterate_to_array(argv[0]);
    int off = argi(argc, argv, 1, 0);
    int type = *(int *)self.o->slot;
    if(off < 0 || off + src->nitems > self.o->nitems) return throw_val(make_error(2, "offset is out of bounds"));
    for(int i = 0; i < src->nitems; i++) self.o->items[off + i] = js_num(ta_conv(type, js_to_num(src->items[i])));
    return js_undef();
}
NATIVE(buffer_ctor){
    (void)ctx;
    js_obj_t *o = ctor_self(self, buffer_proto);
    if(!o) return throw_type("Constructor ArrayBuffer requires 'new'");
    double n = argn(argc, argv, 0); if(n != n || n < 0) n = 0;
    js_obj_t *bytes = new_array();
    for(int i = 0; i < (int)n && i < 64000000; i++) arr_push(bytes, js_num(0));
    o->kind = K_BUFFER; o->slot = bytes;
    js_set(o, "byteLength", js_num(bytes->nitems));
    js_set(o, "_bytes", js_objv(bytes)); find_own(o, "_bytes")->flags = PF_HIDDEN;   /* for DataView (browser_api) */
    return js_objv(o);
}
NATIVE(text_encode){
    (void)self; (void)ctx;
    const unsigned char *s = (const unsigned char *)(argc ? js_to_str(argv[0]) : "");
    u32 L = (u32)strlen((const char *)s);
    js_obj_t *a = make_typed(TA_U8, (int)L);
    for(u32 i = 0; i < L; i++) a->items[i] = js_num(s[i]);
    return js_objv(a);
}
NATIVE(text_decode){
    (void)self; (void)ctx;
    if(argc < 1 || argv[0].t != JS_OBJ) return js_str("");
    js_obj_t *src = argv[0].o->kind == K_BUFFER ? argv[0].o->slot : iterate_to_array(argv[0]);
    char *b = jalloc((u32)src->nitems + 1);
    for(int i = 0; i < src->nitems; i++){ int c = (int)js_to_num(src->items[i]); b[i] = (char)(c ? c : ' '); }
    b[src->nitems] = 0;
    return js_str(b);
}
NATIVE(text_ctor){
    (void)argc; (void)argv;
    js_obj_t *o = self.t == JS_OBJ && self.o != global_obj ? self.o : js_new_object();
    js_set(o, ctx ? "decode" : "encode", js_objv(js_new_native(ctx ? text_decode : text_encode, NULL)));
    js_set(o, "encoding", js_str("utf-8"));
    return js_objv(o);
}

/* ---- Reflect ---- */
NATIVE(reflect_m){
    (void)self;
    js_val_t t = argc > 0 ? argv[0] : js_undef();
    switch((int)(uintptr_t)ctx){
        case 0: return call_spread(t, argc > 1 ? argv[1] : js_undef(), argc > 2 ? argv[2] : js_undef());   /* apply */
        case 1: {                                                                                          /* construct */
            js_obj_t *args = iterate_to_array(argc > 1 ? argv[1] : js_undef());
            return construct(t, args->nitems, args->items);
        }
        case 2: return argc > 1 && t.t == JS_OBJ ? js_get(t.o, js_to_str(argv[1])) : js_undef();            /* get */
        case 3: if(argc > 2 && t.t == JS_OBJ) obj_set(t.o, js_to_str(argv[1]), argv[2]); return js_bool(1); /* set */
        case 4: return js_bool(argc > 1 && t.t == JS_OBJ && has_prop(t.o, js_to_str(argv[1])));            /* has */
        case 5: { js_val_t a[1] = { t }; js_obj_t *r = new_array();                                       /* ownKeys */
                  js_val_t n = obj_get_own_names(js_undef(), 1, a, NULL), sy = obj_get_own_symbols(js_undef(), 1, a, NULL);
                  for(int i = 0; i < n.o->nitems; i++) arr_push(r, n.o->items[i]);
                  for(int i = 0; i < sy.o->nitems; i++) arr_push(r, sy.o->items[i]);
                  return js_objv(r); }
        case 6: if(argc > 1 && t.t == JS_OBJ) prop_remove(t.o, js_to_str(argv[1])); return js_bool(1);     /* deleteProperty */
        case 7: return obj_get_proto2(js_undef(), argc, argv, NULL);
        case 8: obj_set_proto(js_undef(), argc, argv, NULL); return js_bool(1);
        case 9: obj_define_property(js_undef(), argc, argv, NULL); return js_bool(!STOP());
        case 10: return obj_get_own_desc(js_undef(), argc, argv, NULL);
    }
    return js_undef();
}

/* ---- RegExp ---- */
static void *re_jalloc(u32 n){ void *p = jalloc(n); return (char *)p == scratch ? NULL : p; }
/* A regex object for pattern and flags. cache: where a literal keeps its compiled form. */
static js_val_t make_regex(const char *pat, const char *flags, void **cache){
    for(const char *f = flags; *f; f++)
        if(!strchr("gimsuyd", *f) || strchr(f + 1, *f)){
            char m[80] = "Invalid flags supplied to RegExp constructor '"; u32 k = (u32)strlen(m);
            for(const char *q = flags; *q && k < 76; q++) m[k++] = *q;
            m[k++] = '\''; m[k] = 0;
            return throw_val(make_error(3, jstrdup(m, k)));
        }
    void *prog = cache ? *cache : NULL;
    if(!prog){
        const char *err = NULL;
        prog = re_compile(pat, (u32)strlen(pat), flags, re_jalloc, &err);
        if(!prog){
            sb_t m; sb_init(&m, 64);
            sb_puts(&m, "Invalid regular expression: /"); sb_puts(&m, pat); sb_puts(&m, "/: "); sb_puts(&m, err ? err : "error");
            return throw_val(make_error(3, m.b));
        }
        if(cache) *cache = prog;
    }
    js_obj_t *r = new_obj();
    r->is_regex = 1;
    r->proto = regex_proto;
    r->re = jalloc(sizeof(regex_data_t));
    r->re->pat = pat; r->re->plen = (u32)strlen(pat);
    r->re->icase = strchr(flags, 'i') != NULL;
    r->re->global = strchr(flags, 'g') != NULL;
    r->re->sticky = strchr(flags, 'y') != NULL;
    r->re->flags = flags;
    r->re->prog = prog;
    js_set(r, "lastIndex", js_num(0)); find_own(r, "lastIndex")->flags = PF_HIDDEN;
    return js_objv(r);
}
static regex_data_t *rx(js_val_t v){ return v.t == JS_OBJ && v.o->is_regex ? v.o->re : NULL; }
NATIVE(regexp_ctor){
    (void)self; (void)ctx;
    js_val_t p = argc > 0 ? argv[0] : js_undef();
    const char *flags = argc > 1 && argv[1].t != JS_UNDEF ? js_to_str(argv[1]) : NULL;
    regex_data_t *src = rx(p);
    if(src){
        if(!flags) flags = src->flags;
        return make_regex(src->pat, flags, NULL);
    }
    const char *ps = p.t == JS_UNDEF ? "(?:)" : js_to_str(p);
    return make_regex(jstrdup(ps, (u32)strlen(ps)), flags ? jstrdup(flags, (u32)strlen(flags)) : "", NULL);
}
/* Runs the regex at lastIndex (for g/y) or 0. Returns the captures (byte offsets) or NULL. */
static int *regex_run(js_obj_t *r, const char *s, u32 len, int use_last){
    regex_data_t *d = r->re;
    u32 start = 0;
    int gy = d->global || d->sticky;
    if(gy && use_last){
        double li = js_to_num(js_get(r, "lastIndex"));
        if(li != li || li < 0) li = 0;
        if(li > len){ js_set(r, "lastIndex", js_num(0)); return NULL; }
        start = (u32)li;
    }
    int ng = re_ngroups(d->prog);
    int *caps = jalloc(sizeof(int) * (u32)(2 * (ng + 1)));
    int ok = re_exec(d->prog, s, len, start, d->sticky, caps);
    if(ok != 1){ if(gy && use_last) js_set(r, "lastIndex", js_num(0)); return NULL; }
    if(gy && use_last) js_set(r, "lastIndex", js_num(caps[1]));
    return caps;
}
static js_val_t match_result(js_obj_t *r, const char *s, int *caps){
    int ng = re_ngroups(r->re->prog);
    js_obj_t *a = new_array();
    for(int g = 0; g <= ng; g++){
        if(caps[2*g] < 0) arr_push(a, js_undef());
        else arr_push(a, js_str(jstrdup(s + caps[2*g], (u32)(caps[2*g+1] - caps[2*g]))));
    }
    js_set(a, "index", js_num(caps[0]));
    js_set(a, "input", js_str(s));
    js_val_t groups = js_undef();
    for(int g = 1; g <= ng; g++){
        const char *nm = re_group_name(r->re->prog, g);
        if(!nm) continue;
        if(groups.t == JS_UNDEF) groups = js_objv(js_new_object());
        js_set(groups.o, nm, a->items[g]);
    }
    js_set(a, "groups", groups);
    return js_objv(a);
}
NATIVE(regex_exec_m){
    (void)ctx;
    if(!rx(self)) return throw_type("RegExp.prototype.exec called on incompatible receiver");
    const char *s = argc ? js_to_str(argv[0]) : "undefined";
    int *caps = regex_run(self.o, s, (u32)strlen(s), 1);
    if(!caps) return js_null();
    return match_result(self.o, s, caps);
}
NATIVE(regex_test_m){
    (void)ctx;
    if(!rx(self)) return throw_type("RegExp.prototype.test called on incompatible receiver");
    const char *s = argc ? js_to_str(argv[0]) : "undefined";
    return js_bool(regex_run(self.o, s, (u32)strlen(s), 1) != NULL);
}
NATIVE(regex_to_string_m){
    (void)ctx; (void)argc; (void)argv;
    regex_data_t *d = rx(self);
    if(!d) return js_str("/(?:)/");
    sb_t s; sb_init(&s, 32); sb_putc(&s, '/'); sb_puts(&s, d->pat); sb_putc(&s, '/'); sb_puts(&s, d->flags ? d->flags : "");
    return js_str(s.b);
}
NATIVE(regex_prop){
    (void)argc; (void)argv;
    regex_data_t *d = rx(self);
    int what = (int)(uintptr_t)ctx;
    if(!d) return what == 0 ? js_str("(?:)") : what == 1 ? js_str("") : js_undef();
    const char *f = d->flags ? d->flags : "";
    switch(what){
        case 0: return js_str(d->pat);
        case 1: return js_str(f);
        case 2: return js_bool(strchr(f, 'g') != NULL);
        case 3: return js_bool(strchr(f, 'i') != NULL);
        case 4: return js_bool(strchr(f, 'm') != NULL);
        case 5: return js_bool(strchr(f, 'y') != NULL);
        case 6: return js_bool(strchr(f, 'u') != NULL);
        case 7: return js_bool(strchr(f, 's') != NULL);
    }
    return js_undef();
}
static u32 next_cp(const char *s, u32 i, u32 len){
    if(i >= len) return i + 1;
    i++;
    while(i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}
/* replacement text for one match: $$ $& $` $' $n $nn $<name> */
static void expand_replacement(sb_t *o, const char *rep, const char *s, u32 len, int *caps, int ng, js_obj_t *r){
    for(const char *p = rep; *p; p++){
        if(*p != '$' || !p[1]){ sb_putc(o, *p); continue; }
        char c = p[1];
        if(c == '$'){ sb_putc(o, '$'); p++; }
        else if(c == '&'){ sb_putn(o, s + caps[0], (u32)(caps[1] - caps[0])); p++; }
        else if(c == '`'){ sb_putn(o, s, (u32)caps[0]); p++; }
        else if(c == '\''){ if((u32)caps[1] < len) sb_putn(o, s + caps[1], len - (u32)caps[1]); p++; }
        else if(c >= '0' && c <= '9'){
            int g = c - '0', used = 1;
            if(p[2] >= '0' && p[2] <= '9' && g * 10 + (p[2] - '0') <= ng){ g = g * 10 + (p[2] - '0'); used = 2; }
            if(g >= 1 && g <= ng){
                if(caps[2*g] >= 0) sb_putn(o, s + caps[2*g], (u32)(caps[2*g+1] - caps[2*g]));
                p += used;
            } else sb_putc(o, '$');
        } else if(c == '<' && r){
            const char *e = strchr(p + 2, '>');
            if(!e){ sb_putc(o, '$'); continue; }
            for(int g = 1; g <= ng; g++){
                const char *nm = re_group_name(r->re->prog, g);
                if(nm && strlen(nm) == (u32)(e - p - 2) && memcmp(nm, p + 2, (u32)(e - p - 2)) == 0 && caps[2*g] >= 0)
                    sb_putn(o, s + caps[2*g], (u32)(caps[2*g+1] - caps[2*g]));
            }
            p = e;
        } else sb_putc(o, '$');
    }
}
static js_val_t do_replace(js_val_t self, js_val_t pat, js_val_t rep, int all){
    const char *s = this_str(self);
    u32 len = (u32)strlen(s);
    int fn = js_callable(rep);
    const char *rs = fn ? NULL : js_to_str(rep);
    sb_t o; sb_init(&o, len + 16);
    regex_data_t *d = rx(pat);
    if(d){
        if(all && !d->global) return throw_type("replaceAll must be called with a global RegExp");
        int g = d->global;
        int ng = re_ngroups(d->prog);
        u32 pos = 0, from = 0;
        if(g) js_set(pat.o, "lastIndex", js_num(0));
        else if(d->sticky){ double li = js_to_num(js_get(pat.o, "lastIndex")); from = li == li && li > 0 ? (u32)li : 0; }
        int *caps = jalloc(sizeof(int) * (u32)(2 * (ng + 1)));
        while(from <= len && !STOP()){
            int ok = re_exec(d->prog, s, len, from, d->sticky, caps);
            if(ok != 1){ if(d->sticky) js_set(pat.o, "lastIndex", js_num(0)); break; }
            sb_putn(&o, s + pos, (u32)caps[0] - pos);
            if(fn){
                js_val_t *a = jalloc(sizeof(js_val_t) * (u32)(ng + 4));
                int k = 0;
                for(int x = 0; x <= ng; x++) a[k++] = caps[2*x] < 0 ? js_undef() : js_str(jstrdup(s + caps[2*x], (u32)(caps[2*x+1] - caps[2*x])));
                a[k++] = js_num(caps[0]); a[k++] = js_str(s);
                js_val_t named = match_result(pat.o, s, caps);
                js_val_t grp = js_get(named.o, "groups");
                if(grp.t != JS_UNDEF) a[k++] = grp;
                js_val_t r = call_value(rep, js_undef(), k, a);
                if(STOP()) return js_undef();
                sb_puts(&o, js_to_str(r));
            } else expand_replacement(&o, rs, s, len, caps, ng, pat.o);
            pos = (u32)caps[1];
            if(d->sticky && !g) js_set(pat.o, "lastIndex", js_num(caps[1]));
            if(!g) break;
            from = caps[1] == caps[0] ? next_cp(s, (u32)caps[1], len) : (u32)caps[1];
            if(caps[1] == caps[0] && caps[1] < (int)len){ /* an empty match keeps the next char */ }
        }
        if(pos < len) sb_putn(&o, s + pos, len - pos);
        return js_str(o.b);
    }
    const char *ps = js_to_str(pat);
    u32 pl = (u32)strlen(ps);
    u32 pos = 0;
    int caps[2];
    for(u32 i = 0; i <= len; ){
        const char *hit = NULL;
        if(pl == 0) hit = s + i;
        else { const char *h = strstr(s + i, ps); hit = h; }
        if(!hit) break;
        u32 at = (u32)(hit - s);
        sb_putn(&o, s + pos, at - pos);
        caps[0] = (int)at; caps[1] = (int)(at + pl);
        if(fn){
            js_val_t a[3] = { js_str(ps), js_num(at), js_str(s) };
            js_val_t r = call_value(rep, js_undef(), 3, a);
            if(STOP()) return js_undef();
            sb_puts(&o, js_to_str(r));
        } else expand_replacement(&o, rs, s, len, caps, 0, NULL);
        pos = at + pl;
        if(!all) break;
        i = pl ? at + pl : next_cp(s, at, len);
        if(pl == 0 && at < len){ sb_putn(&o, s + at, i - at); pos = i; }
    }
    if(pos < len) sb_putn(&o, s + pos, len - pos);
    return js_str(o.b);
}
NATIVE(str_replace2){ (void)ctx; return do_replace(self, argc > 0 ? argv[0] : js_undef(), argc > 1 ? argv[1] : js_undef(), 0); }
NATIVE(str_replace_all2){ (void)ctx; return do_replace(self, argc > 0 ? argv[0] : js_undef(), argc > 1 ? argv[1] : js_undef(), 1); }
static js_val_t as_regex(js_val_t v, const char *flags){
    if(rx(v)) return v;
    const char *ps = v.t == JS_UNDEF ? "(?:)" : js_to_str(v);
    return make_regex(jstrdup(ps, (u32)strlen(ps)), flags, NULL);
}
NATIVE(str_match2){
    (void)ctx;
    const char *s = this_str(self);
    js_val_t r = as_regex(argc ? argv[0] : js_undef(), "");
    if(STOP()) return js_undef();
    regex_data_t *d = r.o->re;
    if(!d->global) return regex_exec_m(r, 1, (js_val_t[]){ js_str(s) }, NULL);
    js_set(r.o, "lastIndex", js_num(0));
    js_obj_t *out = new_array();
    u32 len = (u32)strlen(s);
    for(;;){
        int *caps = regex_run(r.o, s, len, 1);
        if(!caps) break;
        arr_push(out, js_str(jstrdup(s + caps[0], (u32)(caps[1] - caps[0]))));
        if(caps[1] == caps[0]) js_set(r.o, "lastIndex", js_num(next_cp(s, (u32)caps[1], len)));
    }
    return out->nitems ? js_objv(out) : js_null();
}
NATIVE(str_match_all){
    (void)ctx;
    const char *s = this_str(self);
    js_val_t r0 = argc ? argv[0] : js_undef();
    if(rx(r0) && !rx(r0)->global) return throw_type("String.prototype.matchAll called with a non-global RegExp argument");
    js_val_t r = rx(r0) ? make_regex(rx(r0)->pat, rx(r0)->flags, &rx(r0)->prog) : as_regex(r0, "g");
    if(STOP()) return js_undef();
    js_obj_t *out = new_array();
    u32 len = (u32)strlen(s);
    for(;;){
        int *caps = regex_run(r.o, s, len, 1);
        if(!caps) break;
        arr_push(out, match_result(r.o, s, caps));
        if(caps[1] == caps[0]) js_set(r.o, "lastIndex", js_num(next_cp(s, (u32)caps[1], len)));
    }
    return make_array_iter(out);
}
NATIVE(str_search){
    (void)ctx;
    const char *s = this_str(self);
    js_val_t r = as_regex(argc ? argv[0] : js_undef(), "");
    if(STOP()) return js_undef();
    int ng = re_ngroups(r.o->re->prog);
    int *caps = jalloc(sizeof(int) * (u32)(2 * (ng + 1)));
    if(re_exec(r.o->re->prog, s, (u32)strlen(s), 0, 0, caps) != 1) return js_num(-1);
    return js_num(caps[0]);
}
NATIVE(str_split2){
    (void)ctx;
    const char *s = this_str(self);
    u32 len = (u32)strlen(s);
    js_obj_t *out = new_array();
    u32 lim = argc > 1 && argv[1].t != JS_UNDEF ? to_u32(js_to_num(argv[1])) : 0xFFFFFFFFu;
    if(lim == 0) return js_objv(out);
    js_val_t sep = argc ? argv[0] : js_undef();
    if(sep.t == JS_UNDEF){ arr_push(out, js_str(s)); return js_objv(out); }
    regex_data_t *d = rx(sep);
    if(d){
        int ng = re_ngroups(d->prog);
        int *caps = jalloc(sizeof(int) * (u32)(2 * (ng + 1)));
        if(len == 0){
            if(re_exec(d->prog, s, 0, 0, 1, caps) != 1) arr_push(out, js_str(s));
            return js_objv(out);
        }
        u32 p = 0, q = 0;
        while(q < len && !STOP()){
            if(re_exec(d->prog, s, len, q, 1, caps) != 1){ q = next_cp(s, q, len); continue; }
            u32 e = (u32)caps[1];
            if(e == p || e > len){ q = next_cp(s, q, len); continue; }
            arr_push(out, js_str(jstrdup(s + p, q - p)));
            if((u32)out->nitems >= lim) return js_objv(out);
            for(int g = 1; g <= ng; g++){
                arr_push(out, caps[2*g] < 0 ? js_undef() : js_str(jstrdup(s + caps[2*g], (u32)(caps[2*g+1] - caps[2*g]))));
                if((u32)out->nitems >= lim) return js_objv(out);
            }
            p = e; q = p;
        }
        arr_push(out, js_str(jstrdup(s + p, len - p)));
        return js_objv(out);
    }
    const char *ps = js_to_str(sep);
    u32 pl = (u32)strlen(ps);
    if(pl == 0){
        for(u32 i = 0; i < len && (u32)out->nitems < lim; ){ u32 j = next_cp(s, i, len); arr_push(out, js_str(jstrdup(s + i, j - i))); i = j; }
        return js_objv(out);
    }
    u32 p = 0;
    for(;;){
        const char *h = strstr(s + p, ps);
        if(!h) break;
        arr_push(out, js_str(jstrdup(s + p, (u32)(h - s) - p)));
        if((u32)out->nitems >= lim) return js_objv(out);
        p = (u32)(h - s) + pl;
    }
    arr_push(out, js_str(jstrdup(s + p, len - p)));
    return js_objv(out);
}


/* ---- JSON ---- */
typedef struct { sb_t out; js_val_t replacer; js_obj_t *allow; const char *gap; js_obj_t *stack; int depth; } jsonw_t;
static void json_quote(sb_t *o, const char *s){
    sb_putc(o, '"');
    for(const unsigned char *p = (const unsigned char *)s; *p; p++){
        unsigned char c = *p;
        switch(c){
            case '"': sb_puts(o, "\\\""); break;
            case '\\': sb_puts(o, "\\\\"); break;
            case '\n': sb_puts(o, "\\n"); break;
            case '\r': sb_puts(o, "\\r"); break;
            case '\t': sb_puts(o, "\\t"); break;
            case '\b': sb_puts(o, "\\b"); break;
            case '\f': sb_puts(o, "\\f"); break;
            default:
                if(c < 0x20){ char b[7] = { '\\', 'u', '0', '0', "0123456789abcdef"[c >> 4], "0123456789abcdef"[c & 15], 0 }; sb_putn(o, b, 6); }
                else sb_putc(o, (char)c);
        }
    }
    sb_putc(o, '"');
}
static void json_indent(jsonw_t *w){
    if(!w->gap || !w->gap[0]) return;
    sb_putc(&w->out, '\n');
    for(int i = 0; i < w->depth; i++) sb_puts(&w->out, w->gap);
}
/* Writes holder[key]; returns 0 when the value is skipped (undefined, functions, symbols). */
static int json_write(jsonw_t *w, js_val_t holder, const char *key, js_val_t v){
    if(STOP()) return 0;
    if(v.t == JS_OBJ){
        js_val_t tj = js_get(v.o, "toJSON");
        if(js_callable(tj)){ js_val_t k = js_str(key); v = call_value(tj, v, 1, &k); }
    }
    if(js_callable(w->replacer)){ js_val_t a[2] = { js_str(key), v }; v = call_value(w->replacer, holder, 2, a); }
    if(v.t == JS_OBJ && (v.o->kind == K_NUMBER || v.o->kind == K_STRING || v.o->kind == K_BOOL) && v.o->slot) v = *(js_val_t *)v.o->slot;
    switch(v.t){
        case JS_NULL: sb_puts(&w->out, "null"); return 1;
        case JS_BOOL: sb_puts(&w->out, v.b ? "true" : "false"); return 1;
        case JS_NUM: sb_puts(&w->out, (v.n == v.n && v.n != 1.0/0.0 && v.n != -1.0/0.0) ? js_to_str(v) : "null"); return 1;
        case JS_STR: if(v.s[0] == '\x01') return 0; json_quote(&w->out, v.s); return 1;
        case JS_UNDEF: return 0;
    }
    if(v.o->is_func) return 0;
    for(int i = 0; i < w->stack->nitems; i++)
        if(w->stack->items[i].o == v.o){ throw_type("Converting circular structure to JSON"); return 0; }
    arr_push(w->stack, v);
    w->depth++;
    if(v.o->is_array){
        sb_putc(&w->out, '[');
        for(int i = 0; i < v.o->nitems && !STOP(); i++){
            if(i) sb_putc(&w->out, ',');
            json_indent(w);
            if(!json_write(w, v, idx_str(i), v.o->items[i])) sb_puts(&w->out, "null");
        }
        w->depth--;
        if(v.o->nitems) json_indent(w);
        sb_putc(&w->out, ']');
    } else {
        sb_putc(&w->out, '{');
        js_obj_t *ks = w->allow ? w->allow : keys_of(v);
        int first = 1;
        for(int i = 0; i < ks->nitems && !STOP(); i++){
            const char *k = js_to_str(ks->items[i]);
            js_val_t pv = js_get(v.o, k);
            u32 mark = w->out.n;
            if(!first) sb_putc(&w->out, ',');
            json_indent(w);
            json_quote(&w->out, k);
            sb_putc(&w->out, ':');
            if(w->gap && w->gap[0]) sb_putc(&w->out, ' ');
            if(json_write(w, v, k, pv)) first = 0;
            else { w->out.n = mark; w->out.b[mark] = 0; }
        }
        w->depth--;
        if(!first) json_indent(w);
        sb_putc(&w->out, '}');
    }
    w->stack->nitems--;
    return 1;
}
NATIVE(json_stringify2){
    (void)self; (void)ctx;
    jsonw_t w;
    sb_init(&w.out, 256);
    w.replacer = argc > 1 && js_callable(argv[1]) ? argv[1] : js_undef();
    w.allow = argc > 1 && argv[1].t == JS_OBJ && argv[1].o->is_array ? argv[1].o : NULL;
    w.gap = NULL; w.depth = 0; w.stack = new_array();
    if(argc > 2){
        if(argv[2].t == JS_NUM){
            int n = (int)argv[2].n; if(n > 10) n = 10;
            if(n > 0){ char *g = jalloc((u32)n + 1); memset(g, ' ', (u32)n); g[n] = 0; w.gap = g; }
        } else if(argv[2].t == JS_STR){
            u32 L = (u32)strlen(argv[2].s); if(L > 10) L = 10;
            w.gap = jstrdup(argv[2].s, L);
        }
    }
    js_obj_t *wrap = js_new_object();
    js_val_t v = argc ? argv[0] : js_undef();
    js_set(wrap, "", v);
    if(!json_write(&w, js_objv(wrap), "", v)) return js_undef();
    if(STOP()) return js_undef();
    return js_str(w.out.b);
}
typedef struct { const char *p, *start; int err; } jsonr_t;
static void jr_ws(jsonr_t *r){ while(*r->p == ' ' || *r->p == '\n' || *r->p == '\t' || *r->p == '\r') r->p++; }
static js_val_t jr_value(jsonr_t *r, int depth);
static const char *jr_string(jsonr_t *r){
    r->p++;                                         /* opening quote */
    const char *s0 = r->p;
    int simple = 1;
    while(*r->p && *r->p != '"'){ if(*r->p == '\\'){ simple = 0; r->p++; if(!*r->p) break; } else if((unsigned char)*r->p < 0x20){ r->err = 1; return ""; } r->p++; }
    if(*r->p != '"'){ r->err = 1; return ""; }
    u32 L = (u32)(r->p - s0);
    if(simple){ r->p++; return jstrdup(s0, L); }
    char *out = jalloc(L + 1);
    u32 n = 0;
    for(const char *q = s0; q < r->p; ){
        if(*q != '\\'){ out[n++] = *q++; continue; }
        q++;
        char e = *q++;
        switch(e){
            case 'n': out[n++] = '\n'; break; case 't': out[n++] = '\t'; break; case 'r': out[n++] = '\r'; break;
            case 'b': out[n++] = '\b'; break; case 'f': out[n++] = '\f'; break;
            case '/': case '\\': case '"': out[n++] = e; break;
            case 'u': {
                u32 cp = 0;
                for(int k = 0; k < 4; k++){ int h = hexval(q[k]); if(h < 0){ r->err = 1; return ""; } cp = cp * 16 + (u32)h; }
                q += 4;
                if(cp >= 0xD800 && cp < 0xDC00 && q[0] == '\\' && q[1] == 'u'){
                    u32 lo = 0; int ok = 1;
                    for(int k = 0; k < 4; k++){ int h = hexval(q[2 + k]); if(h < 0){ ok = 0; break; } lo = lo * 16 + (u32)h; }
                    if(ok && lo >= 0xDC00 && lo < 0xE000){ cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
                }
                if(cp == 0) cp = ' ';
                n += put_utf8(out + n, cp);
                break;
            }
            default: r->err = 1; return "";
        }
    }
    out[n] = 0;
    r->p++;
    return out;
}
static js_val_t jr_value(jsonr_t *r, int depth){
    jr_ws(r);
    if(depth > 2000){ r->err = 1; return js_undef(); }
    char c = *r->p;
    if(c == '{'){
        r->p++;
        js_obj_t *o = js_new_object();
        jr_ws(r);
        if(*r->p == '}'){ r->p++; return js_objv(o); }
        for(;;){
            jr_ws(r);
            if(*r->p != '"'){ r->err = 1; return js_undef(); }
            const char *k = jr_string(r);
            if(r->err) return js_undef();
            jr_ws(r);
            if(*r->p != ':'){ r->err = 1; return js_undef(); }
            r->p++;
            js_val_t v = jr_value(r, depth + 1);
            if(r->err) return js_undef();
            if(o->is_array) obj_set(o, k, v); else js_set(o, k, v);
            jr_ws(r);
            if(*r->p == ','){ r->p++; continue; }
            if(*r->p == '}'){ r->p++; return js_objv(o); }
            r->err = 1; return js_undef();
        }
    }
    if(c == '['){
        r->p++;
        js_obj_t *a = new_array();
        jr_ws(r);
        if(*r->p == ']'){ r->p++; return js_objv(a); }
        for(;;){
            js_val_t v = jr_value(r, depth + 1);
            if(r->err) return js_undef();
            arr_push(a, v);
            jr_ws(r);
            if(*r->p == ','){ r->p++; continue; }
            if(*r->p == ']'){ r->p++; return js_objv(a); }
            r->err = 1; return js_undef();
        }
    }
    if(c == '"'){ const char *s = jr_string(r); return js_str(s); }
    if(c == '-' || (c >= '0' && c <= '9')){
        const char *s0 = r->p;
        if(*r->p == '-') r->p++;
        if(!(*r->p >= '0' && *r->p <= '9')){ r->err = 1; return js_undef(); }
        while((*r->p >= '0' && *r->p <= '9') || *r->p == '.' || *r->p == 'e' || *r->p == 'E' ||
              ((*r->p == '+' || *r->p == '-') && (r->p[-1] == 'e' || r->p[-1] == 'E'))) r->p++;
        char tmp[64]; u32 L = (u32)(r->p - s0); if(L > 63) L = 63;
        memcpy(tmp, s0, L); tmp[L] = 0;
        double d = 0; const char *end = NULL;
        kstrtod(tmp, &d, &end);
        return js_num(d);
    }
    if(strncmp(r->p, "true", 4) == 0){ r->p += 4; return js_bool(1); }
    if(strncmp(r->p, "false", 5) == 0){ r->p += 5; return js_bool(0); }
    if(strncmp(r->p, "null", 4) == 0){ r->p += 4; return js_null(); }
    r->err = 1;
    return js_undef();
}
static js_val_t json_revive(js_val_t reviver, js_val_t holder, const char *key){
    js_val_t v = js_get(holder.o, key);
    if(v.t == JS_OBJ){
        js_obj_t *ks = v.o->is_array ? NULL : keys_of(v);
        int n = v.o->is_array ? v.o->nitems : ks->nitems;
        for(int i = 0; i < n && !STOP(); i++){
            const char *k = v.o->is_array ? idx_str(i) : ks->items[i].s;
            js_val_t nv = json_revive(reviver, v, k);
            if(nv.t == JS_UNDEF && !v.o->is_array) prop_remove(v.o, k); else obj_set(v.o, k, nv);
        }
    }
    js_val_t a[2] = { js_str(key), v };
    return call_value(reviver, holder, 2, a);
}
NATIVE(json_parse2){
    (void)self; (void)ctx;
    const char *s = argc ? js_to_str(argv[0]) : "undefined";
    jsonr_t r = { s, s, 0 };
    js_val_t v = jr_value(&r, 0);
    jr_ws(&r);
    if(r.err || *r.p){
        sb_t m; sb_init(&m, 64);
        if(!*r.p) sb_puts(&m, "Unexpected end of JSON input");
        else { sb_puts(&m, "Unexpected token in JSON at position "); sb_puts(&m, num_str((double)(r.p - s))); }
        return throw_val(make_error(3, m.b));
    }
    if(argc > 1 && js_callable(argv[1])){
        js_obj_t *root = js_new_object();
        js_set(root, "", v);
        return json_revive(argv[1], js_objv(root), "");
    }
    return v;
}

/* The text of an uncaught exception (clears the pending throw so toString can run). */
static const char *uncaught_text(void){
    js_val_t tv = thrown_val;
    thrown = 0;
    const char *t;
    t = js_to_str(tv);          /* "TypeError: message" (one line: the stack is not shown) */
    if(failed){ failed = 0; }
    thrown = 1;
    return t[0] ? t : (tv.t == JS_STR ? "(an empty string)" : "(no message)");
}

/* ---- conversions that call script code ---- */
/* ToPrimitive: valueOf/toString (numbers first when want_num). Returns the object if neither works. */
static js_val_t to_primitive(js_val_t v, int want_num){
    if(v.t != JS_OBJ) return v;
    js_obj_t *o = v.o;
    if((o->kind == K_NUMBER || o->kind == K_STRING || o->kind == K_BOOL) && o->slot) return *(js_val_t *)o->slot;
    if(o->kind == K_DATE && o->slot){
        if(want_num) return js_num(*(double *)o->slot);
        return date_to_string(v, 0, NULL, NULL);
    }
    if(depth > MAX_DEPTH - 10) return v;
    const char *order[2] = { want_num ? "valueOf" : "toString", want_num ? "toString" : "valueOf" };
    for(int i = 0; i < 2; i++){
        js_val_t f = js_get(o, order[i]);
        if(!js_callable(f)) continue;
        if(f.o->native == obj_value_of) continue;               /* the default valueOf returns the object */
        if(f.o->native == obj_to_string && !want_num){ return obj_to_string(v, 0, NULL, NULL); }
        js_val_t r = call_value(f, v, 0, NULL);
        if(STOP()) return js_str("");
        if(r.t != JS_OBJ) return r;
    }
    return v;
}
/* for-in: own and inherited enumerable keys */
static js_obj_t *for_in_keys(js_val_t v){
    if(v.t == JS_STR){ js_obj_t *a = new_array(); int n = (int)strlen(v.s); for(int i = 0; i < n; i++) arr_push(a, js_str(idx_str(i))); return a; }
    js_obj_t *out = keys_of(v);
    if(v.t != JS_OBJ) return out;
    for(js_obj_t *x = v.o->proto; x; x = x->proto){
        if(x == object_proto || x == array_proto || x == function_proto) break;
        js_obj_t *ks = keys_of(js_objv(x));
        for(int i = 0; i < ks->nitems; i++){
            int dup = 0;
            for(int j = 0; j < out->nitems && !dup; j++) if(strcmp(out->items[j].s, ks->items[i].s) == 0) dup = 1;
            if(!dup) arr_push(out, ks->items[i]);
        }
    }
    return out;
}

/* ---- wiring ---- */
static void jslib_early(void){
    js_gc_root(&function_proto, sizeof function_proto);
    js_gc_root(&bool_proto, sizeof bool_proto);
    function_proto = new_obj();
    function_proto->proto = object_proto;
    function_proto->is_func = 1;
    function_proto->native = fn_noop;
    bool_proto = new_obj();
    bool_proto->proto = object_proto;
    array_proto->proto = object_proto;
    string_proto->proto = object_proto;
    number_proto->proto = object_proto;
    regex_proto->proto = object_proto;
}
static void move_props(js_obj_t *from, js_obj_t *to){
    for(prop_t *p = from->props; p; p = p->next){ js_set(to, p->k, p->v); find_own(to, p->k)->flags = PF_HIDDEN; }
}
static void accessor(js_obj_t *o, const char *name, js_native_fn get, void *ctx){
    js_set(o, name, js_undef());
    prop_t *p = find_own(o, name);
    p->getter = js_new_native(get, ctx);
    p->flags = PF_HIDDEN;
}
static void jslib_init(void){
    js_obj_t *g = global_obj;
    js_gc_root(&function_proto, sizeof function_proto);
    js_gc_root(&bool_proto, sizeof bool_proto);
    js_gc_root(error_protos, sizeof error_protos);
    js_gc_root(&symbol_registry, sizeof symbol_registry);
    js_gc_root(&iter_proto, sizeof iter_proto);
    js_gc_root(&date_proto, sizeof date_proto);
    js_gc_root(&promise_proto, sizeof promise_proto);
    js_gc_root(&job_queue, sizeof job_queue);
    js_gc_root(&map_proto, sizeof map_proto);
    js_gc_root(&set_proto, sizeof set_proto);
    js_gc_root(&weakmap_proto, sizeof weakmap_proto);
    js_gc_root(&weakset_proto, sizeof weakset_proto);
    js_gc_root(ta_protos, sizeof ta_protos);
    js_gc_root(&buffer_proto, sizeof buffer_proto);

    /* existing natives were made before Function.prototype existed */
    for(prop_t *p = g->props; p; p = p->next){
        if(p->v.t == JS_OBJ && p->v.o->is_func && !p->v.o->proto) p->v.o->proto = function_proto;
        if(p->v.t == JS_OBJ) for(prop_t *q = p->v.o->props; q; q = q->next)
            if(q->v.t == JS_OBJ && q->v.o->is_func && !q->v.o->proto) q->v.o->proto = function_proto;
        p->flags = PF_HIDDEN;
    }
    js_obj_t *protos[] = { array_proto, string_proto, number_proto, regex_proto, object_proto };
    for(int k = 0; k < 5; k++)
        for(prop_t *q = protos[k]->props; q; q = q->next){
            q->flags = PF_HIDDEN;
            if(q->v.t == JS_OBJ && q->v.o->is_func && !q->v.o->proto) q->v.o->proto = function_proto;
        }

    /* Function */
    put_fn(function_proto, "call", fn_call, NULL);
    put_fn(function_proto, "apply", fn_apply, NULL);
    put_fn(function_proto, "bind", fn_bind, NULL);
    put_fn(function_proto, "toString", fn_to_string, NULL);
    js_obj_t *fctor = ctor_named("Function", function_ctor, function_proto);
    (void)fctor;

    /* Object */
    js_obj_t *old_object = js_get(g, "Object").o;
    js_obj_t *object = ctor_named("Object", object_ctor, object_proto);
    if(old_object) move_props(old_object, object);
    put_fn(object, "defineProperty", obj_define_property, NULL);
    put_fn(object, "defineProperties", obj_define_properties, NULL);
    put_fn(object, "getOwnPropertyDescriptor", obj_get_own_desc, NULL);
    put_fn(object, "getOwnPropertyNames", obj_get_own_names, NULL);
    put_fn(object, "getOwnPropertySymbols", obj_get_own_symbols, NULL);
    put_fn(object, "setPrototypeOf", obj_set_proto, NULL);
    put_fn(object, "getPrototypeOf", obj_get_proto2, NULL);
    put_fn(object, "create", obj_create2, NULL);
    put_fn(object, "is", obj_is, NULL);
    put_fn(object, "seal", obj_seal, NULL);
    put_fn(object, "preventExtensions", obj_seal, NULL);
    put_fn(object, "isSealed", obj_false, NULL);
    put_fn(object, "isExtensible", obj_true, NULL);
    put_fn(object, "hasOwn", obj_has_own_static, NULL);
    put_fn(object_proto, "hasOwnProperty", obj_has_own, NULL);
    put_fn(object_proto, "toString", obj_to_string, NULL);
    put_fn(object_proto, "toLocaleString", obj_to_string, NULL);
    put_fn(object_proto, "valueOf", obj_value_of, NULL);
    put_fn(object_proto, "isPrototypeOf", obj_is_proto_of, NULL);
    put_fn(object_proto, "propertyIsEnumerable", obj_prop_is_enum, NULL);
    js_set(object_proto, "__proto__", js_undef());
    { prop_t *p = find_own(object_proto, "__proto__"); p->getter = js_new_native(proto_getter, NULL); p->setter = js_new_native(proto_setter, NULL); p->flags = PF_HIDDEN; }

    /* Errors */
    for(int i = 0; i < 8; i++){
        error_protos[i] = new_with_proto(i == 0 ? object_proto : error_protos[0]);
        js_obj_t *c = ctor_named(error_names[i], error_ctor, error_protos[i]);
        c->ctx = (void *)(uintptr_t)i;
        put_val(error_protos[i], "name", js_str(error_names[i]));
        find_own(error_protos[i], "name")->flags = PF_HIDDEN;
        put_val(error_protos[i], "message", js_str(""));
        find_own(error_protos[i], "message")->flags = PF_HIDDEN;
        if(i == 0){ put_fn(c, "captureStackTrace", error_capture_stack, NULL); put_fn(error_protos[0], "toString", error_to_string, NULL); js_set(c, "stackTraceLimit", js_num(10)); }
    }

    /* Symbol */
    symbol_registry = js_new_object();
    js_obj_t *sym = ctor_named("Symbol", symbol_fn, NULL);
    put_val(sym, "iterator", js_str(SYM_ITERATOR));
    put_val(sym, "asyncIterator", js_str(SYM_ASYNC_ITERATOR));
    put_val(sym, "toStringTag", js_str(SYM_TO_STRING_TAG));
    put_val(sym, "hasInstance", js_str(SYM_HAS_INSTANCE));
    put_val(sym, "toPrimitive", js_str(SYM_TO_PRIMITIVE));
    put_val(sym, "species", js_str("\x01Symbol(Symbol.species)"));
    put_val(sym, "unscopables", js_str("\x01Symbol(Symbol.unscopables)"));
    put_val(sym, "match", js_str("\x01Symbol(Symbol.match)"));
    put_fn(sym, "for", symbol_for, NULL);
    put_fn(string_proto, "description", symbol_to_string, NULL);

    /* Array */
    js_obj_t *old_array = js_get(g, "Array").o;
    js_obj_t *array = ctor_named("Array", array_ctor, array_proto);
    if(old_array) move_props(old_array, array);
    put_fn(array, "from", arr_from2, NULL);
    put_fn(array_proto, "join", arr_join2, NULL);
    put_fn(array_proto, "toString", arr_to_string, NULL);
    put_fn(array_proto, "toLocaleString", arr_to_string, NULL);
    put_fn(array_proto, "splice", arr_splice, NULL);
    put_fn(array_proto, "lastIndexOf", arr_last_index_of, NULL);
    put_fn(array_proto, "reduceRight", arr_reduce_right, NULL);
    put_fn(array_proto, "findLast", arr_find_last, NULL);
    put_fn(array_proto, "findLastIndex", arr_find_last, (void *)1);
    put_fn(array_proto, "copyWithin", arr_copy_within, NULL);
    put_fn(array_proto, "keys", arr_keys, NULL);
    put_fn(array_proto, "values", arr_values, NULL);
    put_fn(array_proto, "entries", arr_entries, NULL);
    put_fn(array_proto, SYM_ITERATOR, arr_values, NULL);
    iter_proto = js_new_object();
    put_fn(iter_proto, SYM_ITERATOR, return_self, NULL);

    /* String */
    js_obj_t *string = js_get(g, "String").o;
    js_set(string, "prototype", js_objv(string_proto)); find_own(string, "prototype")->flags = PF_HIDDEN;
    js_set(string_proto, "constructor", js_objv(string)); find_own(string_proto, "constructor")->flags = PF_HIDDEN;
    put_fn(string, "fromCharCode", str_from_char_code2, NULL);
    put_fn(string, "fromCodePoint", str_from_code_point, NULL);
    put_fn(string_proto, "lastIndexOf", str_last_index_of, NULL);
    put_fn(string_proto, "substr", str_substr, NULL);
    put_fn(string_proto, "codePointAt", str_code_point_at, NULL);
    put_fn(string_proto, "toString", str_self, NULL);
    put_fn(string_proto, "valueOf", str_self, NULL);
    put_fn(string_proto, "normalize", str_self, NULL);
    put_fn(string_proto, "toLocaleLowerCase", js_get(string_proto, "toLowerCase").o->native, NULL);
    put_fn(string_proto, "toLocaleUpperCase", js_get(string_proto, "toUpperCase").o->native, NULL);
    put_fn(string_proto, "trimLeft", js_get(string_proto, "trimStart").o->native, NULL);
    put_fn(string_proto, "trimRight", js_get(string_proto, "trimEnd").o->native, NULL);
    put_fn(string_proto, "localeCompare", str_locale_compare, NULL);
    put_fn(string_proto, "concat", str_concat, NULL);
    put_fn(string_proto, SYM_ITERATOR, arr_values, NULL);

    /* Number, Boolean */
    js_obj_t *number = js_get(g, "Number").o;
    js_set(number, "prototype", js_objv(number_proto)); find_own(number, "prototype")->flags = PF_HIDDEN;
    js_set(number_proto, "constructor", js_objv(number)); find_own(number_proto, "constructor")->flags = PF_HIDDEN;
    put_fn(number, "isInteger", num_is_integer, NULL);
    put_fn(number, "isSafeInteger", num_is_safe_integer, NULL);
    put_fn(number, "isFinite", num_is_finite, NULL);
    put_fn(number, "isNaN", num_is_nan, NULL);
    put_fn(number, "parseFloat", js_get(g, "parseFloat").o->native, NULL);
    put_fn(number, "parseInt", js_get(g, "parseInt").o->native, NULL);
    put_val(number, "MAX_SAFE_INTEGER", js_num(9007199254740991.0));
    put_val(number, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0));
    put_val(number, "EPSILON", js_num(2.220446049250313e-16));
    put_val(number, "MAX_VALUE", js_num(1.7976931348623157e308));
    put_val(number, "MIN_VALUE", js_num(5e-324));
    put_val(number, "POSITIVE_INFINITY", js_num(1.0 / 0.0));
    put_val(number, "NEGATIVE_INFINITY", js_num(-1.0 / 0.0));
    put_val(number, "NaN", js_num(nan_value()));
    put_fn(number_proto, "valueOf", num_value_of, NULL);
    put_fn(number_proto, "toPrecision", num_to_precision, NULL);
    put_fn(number_proto, "toExponential", num_to_exponential, NULL);
    put_fn(number_proto, "toLocaleString", js_get(number_proto, "toString").o->native, NULL);
    js_obj_t *boolean = ctor_named("Boolean", bool_ctor, bool_proto);
    (void)boolean;
    put_fn(bool_proto, "toString", bool_to_string, NULL);
    put_fn(bool_proto, "valueOf", obj_value_of, NULL);
    put_fn(g, "isFinite", g_is_finite, NULL);

    /* Math */
    js_obj_t *math = js_get(g, "Math").o;
    static const char *mnames[] = { "trunc", "sign", "log", "log2", "log10", "exp", "sin", "cos", "tan", "atan", "atan2",
                                    "asin", "acos", "hypot", "cbrt", "imul", "clz32", "fround", "log1p", "expm1", "sinh", "cosh", "tanh", 0 };
    for(int i = 0; mnames[i]; i++) put_fn(math, mnames[i], math_fn2, (void *)(uintptr_t)i);
    put_val(math, "E", js_num(2.718281828459045)); put_val(math, "LN2", js_num(0.6931471805599453));
    put_val(math, "LN10", js_num(2.302585092994046)); put_val(math, "LOG2E", js_num(1.4426950408889634));
    put_val(math, "LOG10E", js_num(0.4342944819032518)); put_val(math, "SQRT2", js_num(1.4142135623730951));
    put_val(math, "SQRT1_2", js_num(0.7071067811865476));
    js_set(math, SYM_TO_STRING_TAG, js_str("Math"));

    /* Date */
    date_proto = js_new_object();
    js_obj_t *date = ctor_named("Date", date_ctor, date_proto);
    put_fn(date, "now", date_now2, NULL);
    put_fn(date, "parse", date_parse, NULL);
    put_fn(date, "UTC", date_utc, NULL);
    static const char *gets[] = { "getTime", "getFullYear", "getMonth", "getDate", "getDay", "getHours", "getMinutes", "getSeconds", "getMilliseconds", "getTimezoneOffset", 0 };
    static const char *ugets[] = { "valueOf", "getUTCFullYear", "getUTCMonth", "getUTCDate", "getUTCDay", "getUTCHours", "getUTCMinutes", "getUTCSeconds", "getUTCMilliseconds", 0 };
    for(int i = 0; gets[i]; i++) put_fn(date_proto, gets[i], date_get, (void *)(uintptr_t)i);
    for(int i = 0; ugets[i]; i++) put_fn(date_proto, ugets[i], date_get, (void *)(uintptr_t)i);
    put_fn(date_proto, "getYear", date_get, (void *)1);
    static const char *sets[] = { "setTime", "setFullYear", "setMonth", "setDate", 0, "setHours", "setMinutes", "setSeconds", "setMilliseconds", 0 };
    static const char *usets[] = { 0, "setUTCFullYear", "setUTCMonth", "setUTCDate", 0, "setUTCHours", "setUTCMinutes", "setUTCSeconds", "setUTCMilliseconds", 0 };
    for(int i = 0; i < 9; i++){ if(sets[i]) put_fn(date_proto, sets[i], date_set, (void *)(uintptr_t)i); if(usets[i]) put_fn(date_proto, usets[i], date_set, (void *)(uintptr_t)i); }
    put_fn(date_proto, "toISOString", date_iso2, NULL);
    put_fn(date_proto, "toJSON", date_iso2, (void *)1);
    put_fn(date_proto, "toString", date_to_string, NULL);
    put_fn(date_proto, "toUTCString", date_to_string, NULL);
    put_fn(date_proto, "toGMTString", date_to_string, NULL);
    put_fn(date_proto, "toDateString", date_to_string, NULL);
    put_fn(date_proto, "toTimeString", date_to_string, NULL);
    put_fn(date_proto, "toLocaleString", date_to_string, NULL);
    put_fn(date_proto, "toLocaleDateString", date_iso2, NULL);
    put_fn(date_proto, "toLocaleTimeString", date_to_string, NULL);

    /* RegExp */
    js_obj_t *regexp = ctor_named("RegExp", regexp_ctor, regex_proto);
    (void)regexp;
    put_fn(regex_proto, "exec", regex_exec_m, NULL);
    put_fn(regex_proto, "test", regex_test_m, NULL);
    put_fn(regex_proto, "toString", regex_to_string_m, NULL);
    static const char *rprops[] = { "source", "flags", "global", "ignoreCase", "multiline", "sticky", "unicode", "dotAll", 0 };
    for(int i = 0; rprops[i]; i++) accessor(regex_proto, rprops[i], regex_prop, (void *)(uintptr_t)i);
    put_fn(string_proto, "replace", str_replace2, NULL);
    put_fn(string_proto, "replaceAll", str_replace_all2, NULL);
    put_fn(string_proto, "match", str_match2, NULL);
    put_fn(string_proto, "matchAll", str_match_all, NULL);
    put_fn(string_proto, "search", str_search, NULL);
    put_fn(string_proto, "split", str_split2, NULL);

    /* JSON */
    js_obj_t *json = js_get(g, "JSON").o;
    put_fn(json, "stringify", json_stringify2, NULL);
    put_fn(json, "parse", json_parse2, NULL);
    js_set(json, SYM_TO_STRING_TAG, js_str("JSON"));

    /* Promise and jobs */
    job_queue = new_array(); job_head = 0;
    promise_proto = js_new_object();
    js_obj_t *promise = ctor_named("Promise", promise_ctor, promise_proto);
    put_fn(promise_proto, "then", promise_then_m, NULL);
    put_fn(promise_proto, "catch", promise_catch_m, NULL);
    put_fn(promise_proto, "finally", promise_finally_m, NULL);
    js_set(promise_proto, SYM_TO_STRING_TAG, js_str("Promise"));
    put_fn(promise, "resolve", promise_resolve_s, NULL);
    put_fn(promise, "reject", promise_reject_s, NULL);
    put_fn(promise, "all", promise_combo, (void *)0);
    put_fn(promise, "allSettled", promise_combo, (void *)1);
    put_fn(promise, "race", promise_combo, (void *)2);
    put_fn(promise, "any", promise_combo, (void *)3);
    put_fn(g, "queueMicrotask", g_queue_microtask, NULL);

    /* Map, Set, WeakMap, WeakSet */
    map_proto = js_new_object(); set_proto = js_new_object(); weakmap_proto = js_new_object(); weakset_proto = js_new_object();
    js_obj_t *mapc = ctor_named("Map", mapset_ctor, map_proto); mapc->ctx = (void *)0;
    js_obj_t *setc = ctor_named("Set", mapset_ctor, set_proto); setc->ctx = (void *)1;
    js_obj_t *wmc = ctor_named("WeakMap", mapset_ctor, weakmap_proto); wmc->ctx = (void *)2;
    js_obj_t *wsc = ctor_named("WeakSet", mapset_ctor, weakset_proto); wsc->ctx = (void *)3;
    static const char *mm[] = { "get", "set", "has", "delete", "clear", 0, "forEach", "keys", "values", "entries" };
    for(int i = 0; i < 10; i++) if(mm[i]){
        put_fn(map_proto, mm[i], map_m, (void *)(uintptr_t)i);
        if(i <= 3) put_fn(weakmap_proto, mm[i], map_m, (void *)(uintptr_t)i);
    }
    accessor(map_proto, "size", map_m, (void *)5);
    put_fn(map_proto, SYM_ITERATOR, map_m, (void *)9);
    js_set(map_proto, SYM_TO_STRING_TAG, js_str("Map"));
    static const char *sm[] = { 0, "add", "has", "delete", "clear", 0, "forEach", "keys", "values", "entries" };
    for(int i = 0; i < 10; i++) if(sm[i]){
        put_fn(set_proto, sm[i], map_m, (void *)(uintptr_t)(i + 100));
        if(i >= 1 && i <= 3) put_fn(weakset_proto, sm[i], map_m, (void *)(uintptr_t)(i + 100));
    }
    accessor(set_proto, "size", map_m, (void *)105);
    put_fn(set_proto, SYM_ITERATOR, map_m, (void *)108);
    js_set(set_proto, SYM_TO_STRING_TAG, js_str("Set"));

    /* typed arrays */
    buffer_proto = js_new_object();
    ctor_named("ArrayBuffer", buffer_ctor, buffer_proto);
    js_obj_t *ta_base = new_with_proto(array_proto);
    accessor(ta_base, "byteLength", typed_byte_length, NULL);
    js_set(ta_base, "byteOffset", js_num(0)); find_own(ta_base, "byteOffset")->flags = PF_HIDDEN;
    put_fn(ta_base, "subarray", typed_subarray, NULL);
    put_fn(ta_base, "set", typed_set_m, NULL);
    for(int i = 0; i < 9; i++){
        ta_protos[i] = new_with_proto(ta_base);
        js_obj_t *c = ctor_named(ta_names[i], typed_ctor, ta_protos[i]);
        c->ctx = (void *)(uintptr_t)i;
        put_val(c, "BYTES_PER_ELEMENT", js_num(ta_size[i]));
        put_val(ta_protos[i], "BYTES_PER_ELEMENT", js_num(ta_size[i]));
        put_fn(c, "from", arr_from2, NULL);
    }
    ctor_named("TextEncoder", text_ctor, js_new_object())->ctx = NULL;
    ctor_named("TextDecoder", text_ctor, js_new_object())->ctx = (void *)1;

    /* Reflect */
    js_obj_t *reflect = js_new_object();
    static const char *rn[] = { "apply", "construct", "get", "set", "has", "ownKeys", "deleteProperty", "getPrototypeOf", "setPrototypeOf", "defineProperty", "getOwnPropertyDescriptor", 0 };
    for(int i = 0; rn[i]; i++) put_fn(reflect, rn[i], reflect_m, (void *)(uintptr_t)i);
    js_set(g, "Reflect", js_objv(reflect)); find_own(g, "Reflect")->flags = PF_HIDDEN;

    /* global functions */
    put_fn(g, "encodeURIComponent", g_encode_uri, NULL);
    put_fn(g, "encodeURI", g_encode_uri, (void *)1);
    put_fn(g, "decodeURIComponent", g_decode_uri, NULL);
    put_fn(g, "decodeURI", g_decode_uri, (void *)1);
    put_fn(g, "escape", g_escape, NULL);
    put_fn(g, "unescape", g_decode_uri, NULL);
    put_fn(g, "btoa", g_btoa, NULL);
    put_fn(g, "atob", g_atob, NULL);
    js_set(g, "undefined", js_undef()); find_own(g, "undefined")->flags = PF_HIDDEN | PF_RO;

}
