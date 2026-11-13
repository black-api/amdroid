#ifndef AMDROID_JS_H
#define AMDROID_JS_H
/* JavaScript for the browser: variables, functions and closures, arrow functions, classes
 * (new, this, super, extends), objects, arrays, strings, regular expressions, if/while/
 * do/for/for-in/for-of/switch, try/catch/finally/throw, the usual operators, timers,
 * template literals, and host objects with get/set hooks. No modules, generators or
 * promises. */
#include "amdroid/types.h"

enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };

typedef struct js_obj js_obj_t;
typedef struct js_val { int t; double n; int b; const char *s; js_obj_t *o; } js_val_t;

typedef js_val_t (*js_native_fn)(js_val_t self, int argc, js_val_t *argv, void *ctx);
/* Host property hooks: get returns 1 and sets *out when it handles the name. */
typedef int (*js_get_fn)(js_obj_t *o, const char *name, js_val_t *out);
typedef int (*js_set_fn)(js_obj_t *o, const char *name, js_val_t v);

void         js_init(void);
void         js_release(void);                    /* frees the script heap */                       /* empties the interpreter (call per page) */
int          js_run(const char *src, u32 len);    /* 0 on success, -1 on error (see js_error) */
const char  *js_error(void);
const char  *js_last_log(void);                   /* the last console.log line */
/* Compiles a function body (with the parameter names given) into a function value. */
js_val_t     js_compile_function(const char *src, u32 len, const char *param);
/* Registers a variable or array (outside the heap) that holds pointers into the heap, so
   they keep their objects alive. The registrations last until the next js_init(). */
void         js_gc_root(void *p, u32 n);
/* Runs the timers that are due at now (100 ticks per second). Returns how many ran. */
int          js_timers_tick(u64 now);
/* 1 if an event handler called preventDefault (or returned false) since the last call. */
int          js_take_prevented(void);
/* Marks the current event as prevented (a handler returned false). */
void         js_mark_prevented(void);
/* A function for event objects' preventDefault. */
js_val_t     js_prevent_default_fn(void);

js_val_t     js_undef(void);
js_val_t     js_null(void);
js_val_t     js_bool(int b);
js_val_t     js_num(double n);
js_val_t     js_str(const char *s);
js_val_t     js_objv(js_obj_t *o);
js_obj_t    *js_global(void);
js_obj_t    *js_new_object(void);
js_obj_t    *js_new_native(js_native_fn fn, void *ctx);
js_obj_t    *js_new_array(void);
void         js_array_push(js_obj_t *a, js_val_t v);
void         js_set(js_obj_t *o, const char *name, js_val_t v);
js_val_t     js_get(js_obj_t *o, const char *name);
void         js_set_hooks(js_obj_t *o, js_get_fn get, js_set_fn set, void *host);
void        *js_host(js_obj_t *o);
js_val_t     js_call(js_val_t fn, js_val_t self, int argc, js_val_t *argv);
int          js_callable(js_val_t v);
const char  *js_to_str(js_val_t v);
double       js_to_num(js_val_t v);
int          js_truthy(js_val_t v);

#endif
