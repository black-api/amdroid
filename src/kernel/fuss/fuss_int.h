/* fuss_int.h — internal types for the in-kernel Fuss interpreter.
 * Language semantics follow the host compiler (fussc) in ~/py/fuss. */
#ifndef AMDROID_FUSS_INT_H
#define AMDROID_FUSS_INT_H
#include "amdroid/types.h"

/* ---------- values ---------- */
enum { V_NUM, V_STR, V_BOOL, V_ARR, V_STRUCT };

typedef struct Val Val;
typedef struct Arr { Val *items; int len, cap; } Arr;
typedef struct Obj { const char *type; const char **keys; Val *vals; int n; } Obj;
struct Val { u8 t; double n; const char *s; Arr *a; Obj *o; };

static inline Val fz_num(double d){ Val v; v.t = V_NUM; v.n = d; v.s = 0; v.a = 0; v.o = 0; return v; }
static inline Val fz_bool(int b){ Val v; v.t = V_BOOL; v.n = b ? 1 : 0; v.s = 0; v.a = 0; v.o = 0; return v; }
static inline Val fz_strv(const char *s){ Val v; v.t = V_STR; v.n = 0; v.s = s; v.a = 0; v.o = 0; return v; }

/* ---------- AST ---------- */
typedef enum {
    N_NUM, N_STR, N_BOOL, N_IDENT, N_BIN, N_UN, N_ARRAYLIT, N_INDEX, N_SETINDEX,
    N_FIELD, N_SETFIELD, N_ASSIGN, N_PRINT, N_IF, N_WHILE, N_FOR, N_BLOCK,
    N_CALL, N_FN, N_RET, N_EXPRSTMT, N_STRUCTDEF, N_STRUCTLIT, N_BREAK, N_CONTINUE, N_EXIT
} NodeType;

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LT, OP_LE, OP_GT, OP_GE,
       OP_EQ, OP_NE, OP_AND, OP_OR, OP_NEG, OP_NOT };

typedef struct Node Node;
struct Node {
    NodeType t;
    double num;
    const char *sval;
    int bval;                 /* N_BOOL value; N_FOR: 1 for range(lo, hi) form */
    int op;                   /* N_BIN / N_UN operator */
    Node *l, *r, *els;
    Node **kids;
    int nkids, cap;
    int line;
    const char *file;
    int local, slot;         /* resolved storage for N_IDENT / N_ASSIGN / N_FOR */
    Node *fn;                 /* N_CALL: target user function (an N_FN node) */
    int builtin;              /* N_CALL: builtin index + 1, or 0 */
    int structidx;            /* N_STRUCTLIT: index into fz_structs */
    /* N_FN only: frame layout. Parameters come first, then locals. */
    int nparams, nlocals, lcap;
    const char **lnames;
};

typedef struct { const char *name; const char **keys; int n; } StructDef;
#define FZ_MAX_STRUCTS 64
#define FZ_MAX_ARGS 8

/* string builder */
typedef struct { char *b; int len, cap; } Sb;
void fz_sb_addn(Sb *sb, const char *s, int n);
void fz_sb_adds(Sb *sb, const char *s);
void fz_sb_addc(Sb *sb, char c);
char *fz_sb_done(Sb *sb);

/* ---------- global state (front.c / run.c) ---------- */
extern Node *fz_program;
extern int fz_nglobals;
extern Val *fz_glob;
extern StructDef fz_structs[FZ_MAX_STRUCTS];
extern int fz_nstructs;
extern const char *fz_file;
extern int fz_line;

/* ---------- errors (run.c) ---------- */
void fz_at(const char *file, int line);
void fz_die(const char *fmt, ...) __attribute__((noreturn)); /* report at fz_file:fz_line and stop */
void fz_finish(int code) __attribute__((noreturn));

/* ---------- front end (front.c) ---------- */
void fz_reset(void);
void fz_compile_main(const char *name, const char *text);
void fz_resolve(void);
int  fz_canon_name(const char *in, char *out, int cap);  /* 8.3 upper-case, or 0 */

/* ---------- files (run.c) ---------- */
int fz_read_fat(const char *name, char **text, u32 *len);

/* ---------- evaluation (eval.c) ---------- */
int   fz_run_program(void);
double fz_numof(Val v);
int   fz_truthy(Val v);
char *fz_tostr(Val v);
void  fz_emit(Sb *sb, Val v, int depth);
void  fz_arr_push(Arr *a, Val v);
Arr  *fz_arr_new(void);

/* ---------- builtins (lib.c) ---------- */
int  fz_builtin_find(const char *name);
int  fz_builtin_argc(int id);     /* -1 = any count */
Val  fz_builtin_call(int id, Val *args, int nargs);

#endif
