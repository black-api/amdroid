#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parser ------------------------------------------------------------ */

typedef struct {
    const char *p, *end;
    char       *err;
    size_t      err_len;
    int         depth;
} parser;

static int fail(parser *ps, const char *msg) {
    if (ps->err && !ps->err[0]) snprintf(ps->err, ps->err_len, "invalid JSON: %s", msg);
    return -1;
}

static void skip_ws(parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int put_utf8(sbuf *b, unsigned cp) {
    char o[4];
    int  n;
    if (cp < 0x80) { o[0] = (char) cp; n = 1; }
    else if (cp < 0x800) { o[0] = (char) (0xC0 | (cp >> 6)); o[1] = (char) (0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        o[0] = (char) (0xE0 | (cp >> 12)); o[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char) (0x80 | (cp & 0x3F)); n = 3;
    } else {
        o[0] = (char) (0xF0 | (cp >> 18)); o[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char) (0x80 | ((cp >> 6) & 0x3F)); o[3] = (char) (0x80 | (cp & 0x3F)); n = 4;
    }
    sb_putn(b, o, (size_t) n);
    return 0;
}

static char *parse_string(parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected string"); return NULL; }
    ps->p++;
    sbuf b;
    sb_init(&b);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if ((unsigned char) c < 0x20) { fail(ps, "control character in string"); sb_free(&b); return NULL; }
        if (c != '\\') { sb_putn(&b, &c, 1); continue; }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
            case '"': sb_putn(&b, "\"", 1); break;
            case '\\': sb_putn(&b, "\\", 1); break;
            case '/': sb_putn(&b, "/", 1); break;
            case 'b': sb_putn(&b, "\b", 1); break;
            case 'f': sb_putn(&b, "\f", 1); break;
            case 'n': sb_putn(&b, "\n", 1); break;
            case 'r': sb_putn(&b, "\r", 1); break;
            case 't': sb_putn(&b, "\t", 1); break;
            case 'u': {
                unsigned cp;
                if (ps->end - ps->p < 4 || hex4(ps->p, &cp)) { fail(ps, "bad \\u escape"); sb_free(&b); return NULL; }
                ps->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    unsigned lo;
                    if (!hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                put_utf8(&b, cp);
                break;
            }
            default: fail(ps, "bad escape"); sb_free(&b); return NULL;
        }
    }
    if (ps->p >= ps->end) { fail(ps, "unterminated string"); sb_free(&b); return NULL; }
    ps->p++;
    sb_putn(&b, "", 0);
    if (!b.s) { b.s = calloc(1, 1); }
    return b.s;
}

static json *parse_value(parser *ps);

static json *new_node(json_type t) {
    json *j = calloc(1, sizeof(json));
    if (j) j->type = t;
    return j;
}

static json *parse_container(parser *ps, int is_obj) {
    json *node = new_node(is_obj ? J_OBJ : J_ARR), *last = NULL;
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == (is_obj ? '}' : ']')) { ps->p++; return node; }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            key = parse_string(ps);
            if (!key) goto bad;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(key); fail(ps, "expected ':'"); goto bad; }
            ps->p++;
        }
        json *v = parse_value(ps);
        if (!v) { free(key); goto bad; }
        v->key = key;
        if (last) last->next = v; else node->child = v;
        last = v;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == (is_obj ? '}' : ']')) { ps->p++; return node; }
        fail(ps, is_obj ? "expected ',' or '}'" : "expected ',' or ']'");
        goto bad;
    }
bad:
    json_free(node);
    return NULL;
}

static json *parse_value(parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
    if (++ps->depth > 64) { fail(ps, "nesting too deep"); return NULL; }
    json *j = NULL;
    char  c = *ps->p;
    if (c == '{' || c == '[') {
        j = parse_container(ps, c == '{');
    } else if (c == '"') {
        char *s = parse_string(ps);
        if (s) { j = new_node(J_STR); j->str = s; }
    } else if (ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) {
        j = new_node(J_BOOL), j->boolean = 1, ps->p += 4;
    } else if (ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) {
        j = new_node(J_BOOL), ps->p += 5;
    } else if (ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) {
        j = new_node(J_NULL), ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char  buf[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof(buf) - 1 && strchr("+-0123456789.eE", *ps->p)) buf[n++] = *ps->p++;
        buf[n] = '\0';
        char *endp;
        double v = strtod(buf, &endp);
        if (endp == buf) fail(ps, "bad number");
        else { j = new_node(J_NUM); j->num = v; }
    } else {
        fail(ps, "unexpected character");
    }
    ps->depth--;
    return j;
}

json *json_parse(const char *s, size_t n, char *err, size_t err_len) {
    parser ps = { s, s + n, err, err_len, 0 };
    if (err && err_len) err[0] = '\0';
    json *j = parse_value(&ps);
    if (!j) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        fail(&ps, "trailing characters");
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(json *j) {
    while (j) {
        json *next = j->next;
        json_free(j->child);
        free(j->str);
        free(j->key);
        free(j);
        j = next;
    }
}

const json *json_get(const json *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (const json *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

double json_num(const json *j, double def) { return j && j->type == J_NUM ? j->num : def; }
int    json_bool(const json *j, int def) { return j && j->type == J_BOOL ? j->boolean : def; }
const char *json_str(const json *j, const char *def) { return j && j->type == J_STR ? j->str : def; }

/* ---- string builder ------------------------------------------------------ */

void sb_init(sbuf *b) { b->s = NULL, b->n = 0, b->cap = 0; }
void sb_free(sbuf *b) { free(b->s); sb_init(b); }

void sb_putn(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        b->s   = realloc(b->s, cap);
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(sbuf *b, const char *fmt, ...) {
    char    tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(tmp)) { sb_putn(b, tmp, (size_t) n); return; }
    char *big = malloc((size_t) n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t) n + 1, fmt, ap);
    va_end(ap);
    sb_putn(b, big, (size_t) n);
    free(big);
}

void sb_json_str(sbuf *b, const char *s, size_t n) {
    sb_putn(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        switch (c) {
            case '"': sb_putn(b, "\\\"", 2); break;
            case '\\': sb_putn(b, "\\\\", 2); break;
            case '\n': sb_putn(b, "\\n", 2); break;
            case '\r': sb_putn(b, "\\r", 2); break;
            case '\t': sb_putn(b, "\\t", 2); break;
            default:
                if (c < 0x20) sb_printf(b, "\\u%04x", c);
                else sb_putn(b, (const char *) &s[i], 1);
        }
    }
    sb_putn(b, "\"", 1);
}
