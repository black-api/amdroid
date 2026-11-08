/* eval.c — tree-walking evaluator for Fuss values, expressions and statements. */
#include "fuss_int.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/kmath.h"
#include <string.h>

enum { X_NORMAL, X_BREAK, X_CONT, X_RET };
static Val fz_retval;

/* ---------- string builder ---------- */
void fz_sb_addn(Sb *sb, const char *s, int n){
    if(sb->len + n + 1 > sb->cap){
        int nc = (sb->len + n + 1) * 2;
        if(nc < 64) nc = 64;
        sb->b = sb->b ? krealloc(sb->b, (size_t)nc) : kmalloc((size_t)nc);
        if(!sb->b) fz_die("out of memory");
        sb->cap = nc;
    }
    memcpy(sb->b + sb->len, s, (size_t)n);
    sb->len += n;
    sb->b[sb->len] = 0;
}
void fz_sb_adds(Sb *sb, const char *s){ fz_sb_addn(sb, s, (int)strlen(s)); }
void fz_sb_addc(Sb *sb, char c){ fz_sb_addn(sb, &c, 1); }
char *fz_sb_done(Sb *sb){
    if(!sb->b){ sb->b = kmalloc(1); if(!sb->b) fz_die("out of memory"); sb->b[0] = 0; sb->cap = 1; }
    return sb->b;
}

/* ---------- arrays ---------- */
Arr *fz_arr_new(void){
    Arr *a = kcalloc(1, sizeof(Arr));
    if(!a) fz_die("out of memory");
    return a;
}
void fz_arr_push(Arr *a, Val v){
    if(a->len == a->cap){
        a->cap = a->cap ? a->cap * 2 : 4;
        a->items = krealloc(a->items, sizeof(Val) * (size_t)a->cap);
        if(!a->items) fz_die("out of memory");
    }
    a->items[a->len++] = v;
}

/* ---------- conversions ---------- */
int fz_truthy(Val v){
    switch(v.t){
        case V_STR: return v.s[0] && strcmp(v.s, "false") != 0 && strcmp(v.s, "0") != 0;
        case V_ARR: return v.a->len > 0;
        case V_STRUCT: return 1;
        default: return v.n != 0;
    }
}

double fz_numof(Val v){
    if(v.t == V_STR){
        double d; const char *end;
        if(!kstrtod(v.s, &d, &end)) fz_die("not a number");
        return d;
    }
    if(v.t == V_STRUCT) fz_die("a struct is not a number");
    if(v.t == V_ARR) fz_die("an array is not a number");
    return v.n;
}

void fz_emit(Sb *sb, Val v, int depth){
    if(depth > 6){ fz_sb_adds(sb, "..."); return; }
    char nb[40];
    switch(v.t){
        case V_NUM: {
            int n = kfmt_double(nb, sizeof nb, v.n);
            fz_sb_addn(sb, nb, n);
            break;
        }
        case V_STR: fz_sb_adds(sb, v.s); break;
        case V_BOOL: fz_sb_adds(sb, v.n ? "true" : "false"); break;
        case V_ARR:
            fz_sb_addc(sb, '[');
            for(int i = 0; i < v.a->len; i++){
                if(i) fz_sb_adds(sb, ", ");
                fz_emit(sb, v.a->items[i], depth + 1);
            }
            fz_sb_addc(sb, ']');
            break;
        case V_STRUCT:
            fz_sb_adds(sb, v.o->type);
            fz_sb_addc(sb, '{');
            for(int i = 0; i < v.o->n; i++){
                if(i) fz_sb_adds(sb, ", ");
                fz_sb_adds(sb, v.o->keys[i]);
                fz_sb_adds(sb, ": ");
                fz_emit(sb, v.o->vals[i], depth + 1);
            }
            fz_sb_addc(sb, '}');
            break;
    }
}

char *fz_tostr(Val v){
    Sb sb = { NULL, 0, 0 };
    fz_emit(&sb, v, 0);
    return fz_sb_done(&sb);
}

/* ---------- operators ---------- */
static int cmp3(Val a, Val b){
    if(a.t == V_ARR || b.t == V_ARR){
        if(a.a == b.a) return 0;
        return ((char*)a.a < (char*)b.a) ? -1 : 1;
    }
    if(a.t == V_STRUCT || b.t == V_STRUCT){
        if(a.o == b.o) return 0;
        return ((char*)a.o < (char*)b.o) ? -1 : 1;
    }
    if(a.t == V_STR && b.t == V_STR) return strcmp(a.s, b.s);
    if(a.t == V_STR || b.t == V_STR) return strcmp(fz_tostr(a), fz_tostr(b));
    if(a.n < b.n) return -1;
    if(a.n > b.n) return 1;
    return 0;
}

static Val concat2(const char *x, const char *y){
    Sb sb = { NULL, 0, 0 };
    fz_sb_adds(&sb, x);
    fz_sb_adds(&sb, y);
    return fz_strv(fz_sb_done(&sb));
}

static Val binop(int op, Val a, Val b){
    switch(op){
        case OP_ADD:
            if(a.t == V_STR && b.t == V_STR) return concat2(a.s, b.s);
            if(a.t == V_STR || b.t == V_STR || a.t >= V_ARR || b.t >= V_ARR)
                return concat2(fz_tostr(a), fz_tostr(b));
            return fz_num(a.n + b.n);
        case OP_SUB: return fz_num(a.n - b.n);
        case OP_MUL: return fz_num(a.n * b.n);
        case OP_DIV:
            if(b.n == 0) fz_die("division by zero");
            return fz_num(a.n / b.n);
        case OP_MOD:
            if(b.n == 0) fz_die("modulo by zero");
            return fz_num(kfmod(a.n, b.n));
        case OP_LT: return fz_bool(cmp3(a, b) < 0);
        case OP_LE: return fz_bool(cmp3(a, b) <= 0);
        case OP_GT: return fz_bool(cmp3(a, b) > 0);
        case OP_GE: return fz_bool(cmp3(a, b) >= 0);
        case OP_EQ: return fz_bool(cmp3(a, b) == 0);
        case OP_NE: return fz_bool(cmp3(a, b) != 0);
    }
    fz_die("internal: bad operator");
    return fz_num(0);
}

/* ---------- indexing, fields ---------- */
static Val index_get(Val o, Val i){
    int idx = (int)fz_numof(i);
    if(o.t == V_ARR){
        if(idx < 0 || idx >= o.a->len) fz_die("array index out of bounds");
        return o.a->items[idx];
    }
    if(o.t == V_STR){
        int n = (int)strlen(o.s);
        if(idx < 0 || idx >= n) fz_die("string index out of bounds");
        char b[2] = { o.s[idx], 0 };
        return fz_strv(strdup(b));
    }
    fz_die("only arrays and strings can be indexed");
    return fz_num(0);
}

static void index_set(Val o, Val i, Val v){
    int idx = (int)fz_numof(i);
    if(o.t != V_ARR) fz_die("only arrays can be index-assigned");
    if(idx < 0 || idx >= o.a->len) fz_die("array index out of bounds (use push() to grow)");
    o.a->items[idx] = v;
}

static int field_index(Val o, const char *name){
    for(int i = 0; i < o.o->n; i++) if(!strcmp(o.o->keys[i], name)) return i;
    fz_die("struct %s has no field '%s'", o.o->type, name);
    return -1;
}

static Val field_get(Val o, const char *name){
    if(o.t != V_STRUCT) fz_die("field access on a non-struct");
    return o.o->vals[field_index(o, name)];
}

static void field_set(Val o, const char *name, Val v){
    if(o.t != V_STRUCT) fz_die("field assignment on a non-struct");
    o.o->vals[field_index(o, name)] = v;
}

/* ---------- variables ---------- */
static Val var_get(Node *n, Val *loc){
    return n->local ? loc[n->slot] : fz_glob[n->slot];
}
static void var_set(Node *n, Val *loc, Val v){
    if(n->local) loc[n->slot] = v;
    else fz_glob[n->slot] = v;
}

/* ---------- evaluation ---------- */
static int exec(Node *n, Val *loc);

static Val fz_eval_node(Node *n, Val *loc);

/* Calls a user function. Arguments are evaluated in the caller's frame and
   stored in the first slots of the new frame (parameters come first). */
static Val call_user(Node *call, Node *fn, Val *loc){
    int nl = fn->nlocals > 0 ? fn->nlocals : 1;
    Val *frame = kcalloc((size_t)nl, sizeof(Val));
    if(!frame) fz_die("out of memory");
    for(int i = 0; i < call->nkids; i++)
        frame[i] = fz_eval_node(call->kids[i], loc);
    int c = exec(fn->l, frame);
    kfree(frame);
    if(c == X_RET) return fz_retval;
    return fz_num(0);
}

static Val fz_eval_node(Node *n, Val *loc){
    if(!n) return fz_num(0);
    switch(n->t){
        case N_NUM: return fz_num(n->num);
        case N_STR: return fz_strv(n->sval);
        case N_BOOL: return fz_bool(n->bval);
        case N_IDENT: return var_get(n, loc);
        case N_BIN: {
            if(n->op == OP_AND){
                Val a = fz_eval_node(n->l, loc);
                if(!fz_truthy(a)) return fz_bool(0);
                Val b = fz_eval_node(n->r, loc);
                return fz_bool(fz_truthy(b));
            }
            if(n->op == OP_OR){
                Val a = fz_eval_node(n->l, loc);
                if(fz_truthy(a)) return fz_bool(1);
                Val b = fz_eval_node(n->r, loc);
                return fz_bool(fz_truthy(b));
            }
            Val a = fz_eval_node(n->l, loc);
            Val b = fz_eval_node(n->r, loc);
            return binop(n->op, a, b);
        }
        case N_UN: {
            Val a = fz_eval_node(n->l, loc);
            if(n->op == OP_NEG) return fz_num(-a.n);
            return fz_bool(!fz_truthy(a));
        }
        case N_ARRAYLIT: {
            Arr *a = fz_arr_new();
            for(int i = 0; i < n->nkids; i++){
                Val v = fz_eval_node(n->kids[i], loc);
                fz_arr_push(a, v);
            }
            Val out; out.t = V_ARR; out.n = 0; out.s = 0; out.a = a; out.o = 0;
            return out;
        }
        case N_INDEX: {
            Val o = fz_eval_node(n->l, loc);
            Val i = fz_eval_node(n->r, loc);
            return index_get(o, i);
        }
        case N_FIELD: {
            Val o = fz_eval_node(n->l, loc);
            return field_get(o, n->sval);
        }
        case N_STRUCTLIT: {
            StructDef *sd = &fz_structs[n->structidx];
            Obj *o = kcalloc(1, sizeof(Obj));
            if(!o) fz_die("out of memory");
            o->type = sd->name;
            o->keys = sd->keys;
            o->n = sd->n;
            o->vals = kcalloc((size_t)(sd->n ? sd->n : 1), sizeof(Val));
            if(!o->vals) fz_die("out of memory");
            for(int i = 0; i < n->nkids; i++){
                Val v = fz_eval_node(n->kids[i], loc);
                o->vals[i] = v;
            }
            Val out; out.t = V_STRUCT; out.n = 0; out.s = 0; out.a = 0; out.o = o;
            return out;
        }
        case N_CALL: {
            if(n->builtin){
                Val args[FZ_MAX_ARGS];
                int na = n->nkids;
                for(int i = 0; i < na; i++) args[i] = fz_eval_node(n->kids[i], loc);
                return fz_builtin_call(n->builtin - 1, args, na);
            }
            return call_user(n, n->fn, loc);
        }
        default:
            fz_die("internal: bad expression");
    }
    return fz_num(0);
}

static void print_line(Node *n, Val *loc){
    Sb sb = { NULL, 0, 0 };
    for(int i = 0; i < n->nkids; i++){
        if(i) fz_sb_addc(&sb, ' ');
        Val v = fz_eval_node(n->kids[i], loc);
        fz_emit(&sb, v, 0);
    }
    fz_sb_addc(&sb, '\n');
    console_puts(fz_sb_done(&sb));
}

static int exec(Node *n, Val *loc){
    if(!n) return X_NORMAL;
    fz_file = n->file;
    fz_line = n->line;
    switch(n->t){
        case N_EXPRSTMT:
            if(n->l) (void)fz_eval_node(n->l, loc);
            return X_NORMAL;
        case N_ASSIGN: {
            Val v = fz_eval_node(n->l, loc);
            var_set(n, loc, v);
            return X_NORMAL;
        }
        case N_SETINDEX: {
            Val o = fz_eval_node(n->l, loc);
            Val i = fz_eval_node(n->r, loc);
            Val v = fz_eval_node(n->els, loc);
            index_set(o, i, v);
            return X_NORMAL;
        }
        case N_SETFIELD: {
            Val o = fz_eval_node(n->l, loc);
            Val v = fz_eval_node(n->r, loc);
            field_set(o, n->sval, v);
            return X_NORMAL;
        }
        case N_PRINT:
            print_line(n, loc);
            return X_NORMAL;
        case N_IF: {
            Val c = fz_eval_node(n->l, loc);
            if(fz_truthy(c)) return exec(n->r, loc);
            if(n->els) return exec(n->els, loc);
            return X_NORMAL;
        }
        case N_WHILE:
            while(1){
                Val c = fz_eval_node(n->l, loc);
                if(!fz_truthy(c)) break;
                int r = exec(n->r, loc);
                if(r == X_BREAK) break;
                if(r == X_RET) return r;
            }
            return X_NORMAL;
        case N_FOR:
            if(n->bval){
                double lo = kfloor(fz_numof(fz_eval_node(n->kids[0], loc)));
                double hi = kfloor(fz_numof(fz_eval_node(n->kids[1], loc)));
                for(double i = lo; i < hi; i += 1){
                    var_set(n, loc, fz_num(i));
                    int r = exec(n->r, loc);
                    if(r == X_BREAK) break;
                    if(r == X_RET) return r;
                }
            } else {
                Val a = fz_eval_node(n->l, loc);
                if(a.t != V_ARR) fz_die("for..in expects an array (or range())");
                for(int i = 0; i < a.a->len; i++){
                    var_set(n, loc, a.a->items[i]);
                    int r = exec(n->r, loc);
                    if(r == X_BREAK) break;
                    if(r == X_RET) return r;
                }
            }
            return X_NORMAL;
        case N_BLOCK:
            for(int i = 0; i < n->nkids; i++){
                int r = exec(n->kids[i], loc);
                if(r != X_NORMAL) return r;
            }
            return X_NORMAL;
        case N_RET:
            fz_retval = n->l ? fz_eval_node(n->l, loc) : fz_num(0);
            return X_RET;
        case N_BREAK: return X_BREAK;
        case N_CONTINUE: return X_CONT;
        case N_EXIT:
            fz_finish(0);
        default:
            return X_NORMAL;
    }
}

int fz_run_program(void){
    for(int i = 0; i < fz_program->nkids; i++){
        int r = exec(fz_program->kids[i], NULL);
        if(r == X_RET) break;   /* a top-level 'ret' ends the program */
    }
    return 0;
}
