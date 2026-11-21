/* Minimal JSON: a DOM parser (for request bodies) and a string builder
 * with JSON escaping (for responses). */
#ifndef THIMBLE_JSON_H
#define THIMBLE_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } json_type;

typedef struct json {
    json_type    type;
    double       num;
    int          boolean;
    char        *str;     /* J_STR value (UTF-8, NUL-terminated) */
    char        *key;     /* member name when inside an object */
    struct json *child;   /* first element / member */
    struct json *next;    /* next sibling */
} json;

/* Parse n bytes; returns NULL and fills err on failure. */
json       *json_parse(const char *s, size_t n, char *err, size_t err_len);
void        json_free(json *j);
const json *json_get(const json *obj, const char *key);   /* NULL if absent / not an object */

/* Typed accessors with defaults (wrong type -> default). */
double      json_num(const json *j, double def);
int         json_bool(const json *j, int def);
const char *json_str(const json *j, const char *def);

/* Growable string buffer. */
typedef struct {
    char  *s;
    size_t n, cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_putn(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
void sb_printf(sbuf *b, const char *fmt, ...);
void sb_json_str(sbuf *b, const char *s, size_t n);   /* quoted + escaped */

#endif
