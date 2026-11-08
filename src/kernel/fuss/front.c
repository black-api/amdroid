/* front.c — Fuss lexer, parser, import handling, and scope resolver.
 * Mirrors the host compiler: an undeclared name used as a value is a string,
 * functions are hoisted, and assigned names inside a function are locals. */
#include "fuss_int.h"
#include "amdroid/heap.h"
#include "amdroid/kmath.h"
#include <string.h>

Node *fz_program;
int fz_nglobals;
Val *fz_glob;
StructDef fz_structs[FZ_MAX_STRUCTS];
int fz_nstructs;

/* ---------- compile-time tables ---------- */
static const char **declared;
static int ndeclared, capdeclared;
static const char **visited;
static int nvisited, capvisited;
static Node **fns;
static int nfns, capfns;
static const char **gnames;
static int ngnames, capgnames;

/* ---------- utilities ---------- */
static int isdig(char c){ return c >= '0' && c <= '9'; }
static int isalph(char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int isalnumc(char c){ return isalph(c) || isdig(c); }

static void *grow(void *p, int *cap, int n, size_t elem){
    if(n < *cap) return p;
    int nc = *cap ? *cap * 2 : 8;
    void *np = krealloc(p, (size_t)nc * elem);
    if(!np) fz_die("out of memory");
    *cap = nc;
    return np;
}

static Node *mk(NodeType t, int line){
    Node *n = kcalloc(1, sizeof(Node));
    if(!n) fz_die("out of memory");
    n->t = t;
    n->line = line;
    n->file = fz_file;
    return n;
}

static void add_kid(Node *p, Node *k){
    if(p->nkids == p->cap){
        p->cap = p->cap ? p->cap * 2 : 4;
        p->kids = krealloc(p->kids, sizeof(Node*) * p->cap);
        if(!p->kids) fz_die("out of memory");
    }
    p->kids[p->nkids++] = k;
}

static int is_declared(const char *name){
    for(int i = 0; i < ndeclared; i++) if(!strcmp(declared[i], name)) return 1;
    return 0;
}
static void add_declared(const char *name){
    if(is_declared(name)) return;
    declared = grow(declared, &capdeclared, ndeclared, sizeof(char*));
    declared[ndeclared++] = name;
}

static int struct_find(const char *name){
    for(int i = 0; i < fz_nstructs; i++) if(!strcmp(fz_structs[i].name, name)) return i;
    return -1;
}

/* Upper-cases a path made of 8.3 components separated by '/'. */
int fz_canon_path(const char *in, char *out, int cap){
    int n = 0;
    const char *p = in;
    if(!*in) return 0;
    for(;;){
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if(len == 0 || len > 12 || (size_t)n + len + 2 > (size_t)cap) return 0;
        for(size_t i = 0; i < len; i++){
            char c = p[i];
            if(c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[n++] = c;
        }
        if(!slash) break;
        out[n++] = '/';
        p = slash + 1;
    }
    out[n] = 0;
    return 1;
}

int fz_canon_name(const char *in, char *out, int cap){
    if(strchr(in, '/')) return 0;
    int n = (int)strlen(in);
    if(n == 0 || n > 12 || n + 1 > cap) return 0;
    for(int i = 0; i < n; i++){
        char c = in[i];
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = c;
    }
    out[n] = 0;
    return 1;
}

/* ---------- lexer ---------- */
typedef enum {
    K_EOF, K_NL, K_IDENT, K_NUM, K_STR,
    K_FUSS, K_EXIT, K_IF, K_ELSE, K_WHILE, K_FN, K_RET, K_PRINT, K_FOR, K_IN,
    K_STRUCT, K_BREAK, K_CONTINUE, K_AND, K_OR, K_NOT, K_TRUE, K_FALSE, K_IMPORT,
    K_ASSIGN, K_EQ, K_NE, K_LT, K_LE, K_GT, K_GE,
    K_PLUS, K_MINUS, K_STAR, K_SLASH, K_PCT,
    K_LP, K_RP, K_LB, K_RB, K_LBK, K_RBK, K_COMMA, K_DOT
} TK;

typedef struct { TK t; const char *s; double n; int line; } Tok;

static const struct { const char *w; TK t; } KEYWORDS[] = {
    {"FUSS", K_FUSS}, {"exit", K_EXIT}, {"if", K_IF}, {"else", K_ELSE},
    {"while", K_WHILE}, {"fn", K_FN}, {"ret", K_RET}, {"print", K_PRINT},
    {"for", K_FOR}, {"in", K_IN}, {"struct", K_STRUCT}, {"break", K_BREAK},
    {"continue", K_CONTINUE}, {"and", K_AND}, {"or", K_OR}, {"not", K_NOT},
    {"true", K_TRUE}, {"false", K_FALSE}, {"import", K_IMPORT},
};

static Tok *lex(const char *s, const char *file, int *outn){
    int cap = 256, n = 0, line = 1, i = 0;
    Tok *out = kmalloc(sizeof(Tok) * cap);
    if(!out) fz_die("out of memory");
    for(;;){
        char c = s[i];
        if(c == ' ' || c == '\t' || c == '\r'){ i++; continue; }
        if(c == '#'){ while(s[i] && s[i] != '\n') i++; continue; }
        Tok t; t.t = K_EOF; t.s = NULL; t.n = 0; t.line = line;
        if(!c){
            out = grow(out, &cap, n, sizeof(Tok));
            out[n++] = t;
            break;
        }
        if(c == '\n'){
            t.t = K_NL; i++;
            out = grow(out, &cap, n, sizeof(Tok));
            out[n++] = t;
            line++;
            continue;
        }
        if(isalph(c)){
            int st = i;
            while(isalnumc(s[i])) i++;
            t.t = K_IDENT;
            t.s = strndup(s + st, (size_t)(i - st));
            for(size_t k = 0; k < sizeof KEYWORDS / sizeof KEYWORDS[0]; k++)
                if(!strcmp(t.s, KEYWORDS[k].w)){ t.t = KEYWORDS[k].t; break; }
        } else if(isdig(c) || (c == '.' && isdig(s[i + 1]))){
            const char *end;
            double v;
            if(!kstrtod(s + i, &v, &end)){ fz_report(file, line, "bad number"); v = 0; end = s + i + 1; }
            i = (int)(end - s);
            t.t = K_NUM; t.n = v;
        } else if(c == '"'){
            int startline = line;
            i++;
            /* literals live in the kernel heap (the AST points at them), not in
               the collected arena, so they use their own buffer */
            int blen = 0, bcap = 16;
            char *bbuf = kmalloc((size_t)bcap);
            if(!bbuf) fz_die("out of memory");
            for(;;){
                char ch = s[i];
                if(!ch || ch == '\n'){ fz_report(file, startline, "unterminated string literal"); break; }
                i++;
                if(ch == '"') break;
                if(ch == '\\'){
                    char e = s[i];
                    if(!e || e == '\n'){ fz_report(file, line, "unterminated string literal"); break; }
                    i++;
                    if(e == 'n') ch = '\n';
                    else if(e == 't') ch = '\t';
                    else if(e == '"') ch = '"';
                    else if(e == '\\') ch = '\\';
                    else { fz_report(file, line, "unknown escape sequence '\\%c' (try \\n \\t \\\" \\\\)", e); ch = e; }
                }
                if(blen + 2 > bcap){ bcap *= 2; bbuf = krealloc(bbuf, (size_t)bcap); if(!bbuf) fz_die("out of memory"); }
                bbuf[blen++] = ch;
            }
            bbuf[blen] = 0;
            t.t = K_STR;
            t.s = bbuf;
        } else {
            i++;
            switch(c){
                case '=': if(s[i] == '='){ i++; t.t = K_EQ; } else t.t = K_ASSIGN; break;
                case '!':
                    if(s[i] == '='){ i++; t.t = K_NE; break; }
                    fz_report(file, line, "unexpected '!' (use 'not' for logical negation)");
                    continue;   /* skip the character and keep lexing */
                case '<': if(s[i] == '='){ i++; t.t = K_LE; } else t.t = K_LT; break;
                case '>': if(s[i] == '='){ i++; t.t = K_GE; } else t.t = K_GT; break;
                case '+': t.t = K_PLUS; break;
                case '-': t.t = K_MINUS; break;
                case '*': t.t = K_STAR; break;
                case '/': t.t = K_SLASH; break;
                case '%': t.t = K_PCT; break;
                case '(': t.t = K_LP; break;
                case ')': t.t = K_RP; break;
                case '{': t.t = K_LB; break;
                case '}': t.t = K_RB; break;
                case '[': t.t = K_LBK; break;
                case ']': t.t = K_RBK; break;
                case ',': t.t = K_COMMA; break;
                case '.': t.t = K_DOT; break;
                default:
                    fz_report(file, line, "unexpected character '%c' (0x%x)", c, (unsigned)(unsigned char)c);
                    continue;
            }
        }
        out = grow(out, &cap, n, sizeof(Tok));
        out[n++] = t;
    }
    *outn = n;
    return out;
}

/* ---------- parser ---------- */
typedef struct { Tok *toks; int n, pos; const char *file; int depth; } Parser;
static Parser *P;
static void **cur_rec;      /* recovery point of the innermost top-level loop */
static int errdepth;        /* open blocks when the last syntax error happened */

static Tok *cur(void){ return &P->toks[P->pos]; }
static TK peek(void){ return P->toks[P->pos].t; }
static TK peek2(void){ return P->pos + 1 < P->n ? P->toks[P->pos + 1].t : K_EOF; }
static void adv(void){ if(P->pos < P->n - 1) P->pos++; }
static int accept(TK t){ if(peek() == t){ adv(); return 1; } return 0; }
static void skipnl(void){ while(peek() == K_NL) adv(); }

/* Records a syntax error and jumps back to the top-level loop, which skips
   to the end of the broken statement and keeps parsing. */
static void perr(const char *msg) __attribute__((noreturn));
static void perr(const char *msg){
    errdepth = P->depth;
    fz_report(P->file, cur()->line, "%s", msg);
    if(cur_rec) __builtin_longjmp(cur_rec, 1);
    fz_finish(1);
}

static void expect(TK t, const char *msg){
    if(peek() != t) perr(msg);
    adv();
}

static Node *parse_expr(void);
static Node *parse_block(void);
static Node *parse_statement(void);

static Node *parse_call(const char *name, int line){
    adv();   /* '(' */
    int sidx = struct_find(name);
    Node *call = mk(sidx >= 0 ? N_STRUCTLIT : N_CALL, line);
    call->sval = name;
    call->structidx = sidx;
    skipnl();
    if(peek() != K_RP){
        for(;;){
            add_kid(call, parse_expr());
            skipnl();
            if(!accept(K_COMMA)) break;
            skipnl();
            if(peek() == K_RP) break;
        }
    }
    if(peek() != K_RP) perr("expected ')' to close call");
    adv();
    return call;
}

static Node *parse_primary(void){
    Tok *t = cur();
    int line = t->line;
    switch(t->t){
        case K_NUM: { Node *n = mk(N_NUM, line); n->num = t->n; adv(); return n; }
        case K_STR: { Node *n = mk(N_STR, line); n->sval = t->s; adv(); return n; }
        case K_TRUE: { Node *n = mk(N_BOOL, line); n->bval = 1; adv(); return n; }
        case K_FALSE: { Node *n = mk(N_BOOL, line); n->bval = 0; adv(); return n; }
        case K_LBK: {
            adv();
            Node *a = mk(N_ARRAYLIT, line);
            skipnl();
            if(peek() != K_RBK){
                for(;;){
                    add_kid(a, parse_expr());
                    skipnl();
                    if(!accept(K_COMMA)) break;
                    skipnl();
                }
            }
            if(peek() != K_RBK) perr("expected ']' to close array literal");
            adv();
            return a;
        }
        case K_IDENT: {
            const char *name = t->s;
            adv();
            if(peek() == K_LP) return parse_call(name, line);
            Node *n = mk(is_declared(name) ? N_IDENT : N_STR, line);
            n->sval = name;
            return n;
        }
        case K_LP: {
            adv();
            Node *e = parse_expr();
            if(peek() != K_RP) perr("expected ')' to close expression");
            adv();
            return e;
        }
        default:
            perr("expected an expression");
    }
    return NULL;
}

static Node *parse_postfix(Node *n){
    for(;;){
        if(peek() == K_LBK){
            adv();
            Node *idx = parse_expr();
            if(peek() != K_RBK) perr("expected ']' to close index");
            adv();
            Node *ix = mk(N_INDEX, n->line);
            ix->l = n; ix->r = idx;
            n = ix;
            continue;
        }
        if(peek() == K_DOT){
            adv();
            if(peek() != K_IDENT) perr("expected field name after '.'");
            Node *f = mk(N_FIELD, cur()->line);
            f->l = n;
            f->sval = cur()->s;
            adv();
            n = f;
            continue;
        }
        return n;
    }
}

static Node *parse_unary(void){
    if(peek() == K_MINUS){
        int line = cur()->line; adv();
        Node *n = mk(N_UN, line); n->op = OP_NEG; n->l = parse_unary(); return n;
    }
    if(peek() == K_NOT){
        int line = cur()->line; adv();
        Node *n = mk(N_UN, line); n->op = OP_NOT; n->l = parse_unary(); return n;
    }
    if(peek() == K_PLUS){ adv(); return parse_unary(); }
    return parse_postfix(parse_primary());
}

static Node *bin(int op, Node *l, Node *r){
    Node *n = mk(N_BIN, l->line);
    n->op = op; n->l = l; n->r = r;
    return n;
}

static Node *parse_mul(void){
    Node *n = parse_unary();
    for(;;){
        if(accept(K_STAR)) n = bin(OP_MUL, n, parse_unary());
        else if(accept(K_SLASH)) n = bin(OP_DIV, n, parse_unary());
        else if(accept(K_PCT)) n = bin(OP_MOD, n, parse_unary());
        else return n;
    }
}
static Node *parse_add(void){
    Node *n = parse_mul();
    for(;;){
        if(accept(K_PLUS)) n = bin(OP_ADD, n, parse_mul());
        else if(accept(K_MINUS)) n = bin(OP_SUB, n, parse_mul());
        else return n;
    }
}
static Node *parse_cmp(void){
    Node *n = parse_add();
    for(;;){
        if(accept(K_LT)) n = bin(OP_LT, n, parse_add());
        else if(accept(K_LE)) n = bin(OP_LE, n, parse_add());
        else if(accept(K_GT)) n = bin(OP_GT, n, parse_add());
        else if(accept(K_GE)) n = bin(OP_GE, n, parse_add());
        else return n;
    }
}
static Node *parse_eq(void){
    Node *n = parse_cmp();
    for(;;){
        if(accept(K_EQ)) n = bin(OP_EQ, n, parse_cmp());
        else if(accept(K_NE)) n = bin(OP_NE, n, parse_cmp());
        else return n;
    }
}
static Node *parse_and(void){
    Node *n = parse_eq();
    while(accept(K_AND)) n = bin(OP_AND, n, parse_eq());
    return n;
}
static Node *parse_or(void){
    Node *n = parse_and();
    while(accept(K_OR)) n = bin(OP_OR, n, parse_and());
    return n;
}
static Node *parse_expr(void){ return parse_or(); }

static Node *parse_block(void){
    expect(K_LB, "'{'");
    Node *block = mk(N_BLOCK, cur()->line);
    P->depth++;
    skipnl();
    while(peek() != K_RB && peek() != K_EOF){
        Node *s = parse_statement();
        if(s) add_kid(block, s);
        skipnl();
    }
    if(peek() != K_RB) perr("expected '}' to close block");
    adv();
    P->depth--;
    return block;
}

static Node *parse_statement(void){
    skipnl();
    Tok *t = cur();
    int line = t->line;
    switch(t->t){
        case K_EOF: return NULL;
        case K_FUSS: adv(); return NULL;
        case K_IMPORT: perr("import must be at the top level of a file");
        case K_EXIT: adv(); return mk(N_EXIT, line);
        case K_BREAK: adv(); return mk(N_BREAK, line);
        case K_CONTINUE: adv(); return mk(N_CONTINUE, line);
        case K_PRINT: {
            adv();
            expect(K_LP, "'(' after print");
            Node *p = mk(N_PRINT, line);
            skipnl();
            if(peek() != K_RP){
                for(;;){
                    add_kid(p, parse_expr());
                    skipnl();
                    if(!accept(K_COMMA)) break;
                    skipnl();
                    if(peek() == K_RP) break;
                }
            }
            if(peek() != K_RP) perr("expected ')' to close print");
            adv();
            return p;
        }
        case K_IF: {
            adv();
            Node *n = mk(N_IF, line);
            n->l = parse_expr();
            if(peek() != K_LB) perr("expected '{' after if-condition");
            n->r = parse_block();
            skipnl();
            if(accept(K_ELSE)){
                skipnl();
                if(peek() == K_IF) n->els = parse_statement();
                else if(peek() == K_LB) n->els = parse_block();
                else perr("expected '{' or 'if' after else");
            }
            return n;
        }
        case K_WHILE: {
            adv();
            Node *n = mk(N_WHILE, line);
            n->l = parse_expr();
            if(peek() != K_LB) perr("expected '{' after while-condition");
            n->r = parse_block();
            return n;
        }
        case K_FOR: {
            adv();
            if(peek() != K_IDENT) perr("expected iterator variable after 'for'");
            const char *iter = cur()->s;
            int il = cur()->line;
            adv();
            expect(K_IN, "'in' after for-iterator");
            Node *it = parse_expr();
            add_declared(iter);
            Node *n = mk(N_FOR, il);
            n->sval = iter;
            if(it->t == N_CALL && !strcmp(it->sval, "range") && it->nkids == 2){
                n->bval = 1;
                n->kids = it->kids;
                n->nkids = 2;
            } else {
                n->l = it;
            }
            if(peek() != K_LB) perr("expected '{' after for-expression");
            n->r = parse_block();
            return n;
        }
        case K_STRUCT: {
            adv();
            if(peek() != K_IDENT) perr("expected struct name");
            const char *sname = cur()->s;
            adv();
            expect(K_LB, "'{' after struct name");
            const char *fields[64];
            int nf = 0;
            skipnl();
            if(peek() != K_RB){
                for(;;){
                    if(peek() != K_IDENT) perr("expected field name");
                    if(nf >= 64) perr("too many struct fields (max 64)");
                    fields[nf++] = cur()->s;
                    adv();
                    skipnl();
                    if(!accept(K_COMMA)) break;
                    skipnl();
                    if(peek() == K_RB) break;
                }
            }
            if(peek() != K_RB) perr("expected '}' to close struct");
            adv();
            if(struct_find(sname) >= 0){ fz_report(P->file, line, "struct '%s' is already defined", sname); return NULL; }
            if(fz_nstructs == FZ_MAX_STRUCTS){ fz_report(P->file, line, "too many structs (max %d)", FZ_MAX_STRUCTS); return NULL; }
            StructDef *sd = &fz_structs[fz_nstructs++];
            sd->name = sname;
            sd->n = nf;
            sd->keys = kmalloc(sizeof(char*) * (nf ? nf : 1));
            if(!sd->keys) fz_die("out of memory");
            for(int i = 0; i < nf; i++) sd->keys[i] = fields[i];
            return NULL;
        }
        case K_FN: {
            adv();
            if(peek() != K_IDENT) perr("expected function name");
            const char *fname = cur()->s;
            int fl = cur()->line;
            adv();
            expect(K_LP, "'(' after function name");
            Node *fn = mk(N_FN, fl);
            fn->sval = fname;
            add_declared(fname);
            skipnl();
            if(peek() != K_RP){
                for(;;){
                    if(peek() != K_IDENT) perr("expected parameter name");
                    Node *p = mk(N_IDENT, cur()->line);
                    p->sval = cur()->s;
                    add_kid(fn, p);
                    add_declared(p->sval);
                    adv();
                    skipnl();
                    if(!accept(K_COMMA)) break;
                    skipnl();
                    if(peek() == K_RP) break;
                }
            }
            if(peek() != K_RP) perr("expected ')' to close parameters");
            adv();
            if(peek() != K_LB) perr("expected '{' to open function body");
            fn->l = parse_block();
            return fn;
        }
        case K_RET: {
            adv();
            Node *n = mk(N_RET, line);
            if(peek() != K_NL && peek() != K_RB && peek() != K_EOF) n->l = parse_expr();
            return n;
        }
        case K_IDENT:
            if(peek2() == K_ASSIGN){
                const char *name = t->s;
                adv(); adv();
                Node *n = mk(N_ASSIGN, line);
                n->sval = name;
                n->l = parse_expr();
                add_declared(name);
                return n;
            }
            break;
        default:
            break;
    }
    Node *e = parse_expr();
    if(peek() == K_ASSIGN){
        if(e->t == N_INDEX){
            adv();
            Node *s = mk(N_SETINDEX, e->line);
            s->l = e->l; s->r = e->r;
            s->els = parse_expr();
            return s;
        }
        if(e->t == N_FIELD){
            adv();
            Node *s = mk(N_SETFIELD, e->line);
            s->l = e->l; s->sval = e->sval;
            s->r = parse_expr();
            return s;
        }
        perr("cannot assign to this expression");
    }
    Node *st = mk(N_EXPRSTMT, line);
    st->l = e;
    return st;
}

/* Parses a whole source file into the program, expanding imports in place. */
static void parse_source(const char *name, const char *text);

/* After a syntax error: skip to the end of the broken statement. Tokens are
   skipped while tracking braces, so a bad function body is skipped whole. */
static void recover(void){
    int open = 0, ed = errdepth;
    for(;;){
        TK t = peek();
        if(t == K_EOF) break;
        if(t == K_LB) open++;
        else if(t == K_RB){
            if(open > 0) open--;
            else if(ed > 0) ed--;
        } else if(t == K_NL && open == 0 && ed == 0) break;
        adv();
    }
    P->depth = 0;
}

static void toplevel_loop(void){
    void *jb[5];
    void **prev = cur_rec;
    cur_rec = jb;
    if(__builtin_setjmp(jb)){
        recover();
    }
    for(;;){
        skipnl();
        if(peek() == K_EOF) break;
        if(peek() == K_IMPORT){
            int line = cur()->line;
            adv();
            if(peek() != K_STR) perr("expected a file name in quotes after import");
            const char *fname = cur()->s;
            adv();
            /* relative to the importing file's directory, like the host compiler */
            char full[80], canon[80];
            const char *sl = strrchr(P->file, '/');
            size_t dl = sl ? (size_t)(sl - P->file) + 1 : 0;
            int fits = dl + strlen(fname) + 1 <= sizeof full;
            if(fits){ memcpy(full, P->file, dl); strcpy(full + dl, fname); }
            if(!fits || !fz_canon_path(full, canon, sizeof canon)){
                fz_report(P->file, line, "cannot import '%s' (use 8.3 names separated by '/')", fname);
                continue;
            }
            int seen = 0;
            for(int i = 0; i < nvisited; i++) if(!strcmp(visited[i], canon)) seen = 1;
            if(seen) continue;
            visited = grow(visited, &capvisited, nvisited, sizeof(char*));
            visited[nvisited++] = strdup(canon);
            char *text; u32 len;
            if(fz_read_fat(canon, &text, &len) != 0){
                fz_report(P->file, line, "cannot open imported file '%s'", fname);
                continue;
            }
            parse_source(canon, text);
            continue;
        }
        Node *s = parse_statement();
        if(s) add_kid(fz_program, s);
    }
    cur_rec = prev;
}

static void parse_toplevel(Parser *p){
    Parser *saved = P;
    P = p;
    toplevel_loop();
    P = saved;
}

static void parse_source(const char *name, const char *text){
    const char *file = strdup(name);
    fz_file = file;
    fz_line = 1;
    int n;
    Tok *toks = lex(text, file, &n);
    Parser p = { toks, n, 0, file, 0 };
    parse_toplevel(&p);
}

void fz_compile_main(const char *name, const char *text){
    char canon[80];
    if(fz_canon_path(name, canon, sizeof canon)){
        visited = grow(visited, &capvisited, nvisited, sizeof(char*));
        visited[nvisited++] = strdup(canon);
        parse_source(canon, text);
    } else {
        parse_source(name, text);
    }
}

void fz_reset(void){
    fz_eval_reset();
    ndeclared = 0; nvisited = 0; nfns = 0; ngnames = 0; fz_nstructs = 0; fz_nglobals = 0;
    fz_program = mk(N_BLOCK, 1);
    fz_program->file = "";
}

/* ---------- resolver: slots, arity, loop placement ---------- */
static int fn_find(const char *name){
    for(int i = 0; i < nfns; i++) if(!strcmp(fns[i]->sval, name)) return i;
    return -1;
}

static int gslot(const char *name){
    for(int i = 0; i < ngnames; i++) if(!strcmp(gnames[i], name)) return i;
    gnames = grow(gnames, &capgnames, ngnames, sizeof(char*));
    gnames[ngnames] = name;
    return ngnames++;
}

static int lfind(Node *fn, const char *name){
    for(int i = 0; i < fn->nlocals; i++) if(!strcmp(fn->lnames[i], name)) return i;
    return -1;
}

static void ladd_raw(Node *fn, const char *name){
    if(fn->nlocals == fn->lcap){
        fn->lcap = fn->lcap ? fn->lcap * 2 : 8;
        fn->lnames = krealloc(fn->lnames, sizeof(char*) * fn->lcap);
        if(!fn->lnames) fz_die("out of memory");
    }
    fn->lnames[fn->nlocals++] = name;
}
static void ladd(Node *fn, const char *name){
    if(lfind(fn, name) < 0) ladd_raw(fn, name);
}

static void collect_locals(Node *fn, Node *n){
    if(!n) return;
    if(n->t == N_ASSIGN || n->t == N_FOR) ladd(fn, n->sval);
    collect_locals(fn, n->l);
    collect_locals(fn, n->r);
    collect_locals(fn, n->els);
    for(int i = 0; i < n->nkids; i++) collect_locals(fn, n->kids[i]);
}

static void bind(Node *n, Node *fn){
    if(fn){
        int i = lfind(fn, n->sval);
        if(i >= 0){ n->local = 1; n->slot = i; return; }
    }
    n->local = 0;
    n->slot = gslot(n->sval);
}

static void resolve(Node *n, Node *fn, int loopd){
    if(!n) return;
    switch(n->t){
        case N_IDENT:
            bind(n, fn);
            return;
        case N_ASSIGN:
            bind(n, fn);
            resolve(n->l, fn, loopd);
            return;
        case N_FOR:
            bind(n, fn);
            if(n->bval){
                resolve(n->kids[0], fn, loopd);
                resolve(n->kids[1], fn, loopd);
            } else {
                resolve(n->l, fn, loopd);
            }
            resolve(n->r, fn, loopd + 1);
            return;
        case N_WHILE:
            resolve(n->l, fn, loopd);
            resolve(n->r, fn, loopd + 1);
            return;
        case N_BREAK:
            if(loopd == 0){ fz_report(n->file, n->line, "'break' outside of a loop"); }
            return;
        case N_CONTINUE:
            if(loopd == 0){ fz_report(n->file, n->line, "'continue' outside of a loop"); }
            return;
        case N_FN:
            fz_report(n->file, n->line, "functions must be defined at the top level");
            return;
        case N_STRUCTDEF:
            return;
        case N_CALL: {
            int f = fn_find(n->sval);
            if(f >= 0){
                Node *target = fns[f];
                if(n->nkids != target->nparams){
                    fz_report(n->file, n->line, "function '%s' takes %d argument(s), got %d", n->sval, target->nparams, n->nkids);
                }
                n->fn = target;
            } else {
                int b = fz_builtin_find(n->sval);
                if(b < 0){
                    fz_report(n->file, n->line, "unknown function '%s' (not defined and not a builtin)", n->sval);
                    for(int i = 0; i < n->nkids; i++) resolve(n->kids[i], fn, loopd);
                    return;
                }
                int want = fz_builtin_argc(b);
                if(want >= 0 && n->nkids != want){
                    fz_report(n->file, n->line, "builtin '%s' takes %d argument(s), got %d", n->sval, want, n->nkids);
                }
                if(n->nkids > FZ_MAX_ARGS){ fz_report(n->file, n->line, "too many arguments to '%s'", n->sval); }
                n->builtin = b + 1;
            }
            for(int i = 0; i < n->nkids; i++) resolve(n->kids[i], fn, loopd);
            return;
        }
        case N_STRUCTLIT: {
            if(n->structidx < 0){ fz_report(n->file, n->line, "unknown struct '%s'", n->sval); }
            StructDef *sd = &fz_structs[n->structidx];
            if(n->nkids != sd->n){
                fz_report(n->file, n->line, "struct '%s' takes %d field(s), got %d", sd->name, sd->n, n->nkids);
            }
            for(int i = 0; i < n->nkids; i++) resolve(n->kids[i], fn, loopd);
            return;
        }
        default:
            resolve(n->l, fn, loopd);
            resolve(n->r, fn, loopd);
            resolve(n->els, fn, loopd);
            for(int i = 0; i < n->nkids; i++) resolve(n->kids[i], fn, loopd);
            return;
    }
}

void fz_resolve(void){
    /* pass 1: hoist functions and lay out their frames */
    for(int i = 0; i < fz_program->nkids; i++){
        Node *k = fz_program->kids[i];
        if(k->t != N_FN) continue;
        if(fn_find(k->sval) >= 0){ fz_report(k->file, k->line, "function '%s' is already defined", k->sval); continue; }
        fns = grow(fns, &capfns, nfns, sizeof(Node*));
        fns[nfns++] = k;
        for(int p = 0; p < k->nkids; p++) ladd_raw(k, k->kids[p]->sval);
        collect_locals(k, k->l);
        k->nparams = k->nkids;
    }
    /* pass 2: bind every name and check calls, structs, and loops */
    for(int i = 0; i < fz_program->nkids; i++){
        Node *k = fz_program->kids[i];
        if(k->t == N_FN) resolve(k->l, k, 0);
        else resolve(k, NULL, 0);
    }
    fz_nglobals = ngnames;
}
