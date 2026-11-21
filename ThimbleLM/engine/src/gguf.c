#include "gguf.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int            bad;
} cursor;

static int need(cursor *c, size_t n) {
    if (c->bad || (size_t) (c->end - c->p) < n) {
        c->bad = 1;
        return 0;
    }
    return 1;
}

static uint32_t rd_u32(cursor *c) {
    uint32_t v = 0;
    if (need(c, 4)) {
        memcpy(&v, c->p, 4);
        c->p += 4;
    }
    return v;
}

static uint64_t rd_u64(cursor *c) {
    uint64_t v = 0;
    if (need(c, 8)) {
        memcpy(&v, c->p, 8);
        c->p += 8;
    }
    return v;
}

static gguf_str rd_str(cursor *c) {
    gguf_str s = { NULL, 0 };
    s.n = rd_u64(c);
    if (need(c, s.n)) {
        s.s = (const char *) c->p;
        c->p += s.n;
    }
    return s;
}

static size_t scalar_size(uint32_t t) {
    switch (t) {
        case GGUF_UINT8: case GGUF_INT8: case GGUF_BOOL: return 1;
        case GGUF_UINT16: case GGUF_INT16: return 2;
        case GGUF_UINT32: case GGUF_INT32: case GGUF_FLOAT32: return 4;
        case GGUF_UINT64: case GGUF_INT64: case GGUF_FLOAT64: return 8;
        default: return 0;
    }
}

static void skip_value(cursor *c, uint32_t t) {
    if (t == GGUF_STRING) {
        rd_str(c);
    } else if (t == GGUF_ARRAY) {
        uint32_t at = rd_u32(c);
        uint64_t n  = rd_u64(c);
        if (at == GGUF_ARRAY) { c->bad = 1; return; }
        if (at == GGUF_STRING) {
            for (uint64_t i = 0; i < n && !c->bad; i++) rd_str(c);
        } else {
            size_t sz = scalar_size(at);
            if (!sz || n > (uint64_t) (c->end - c->p) / sz) { c->bad = 1; return; }
            c->p += sz * n;
        }
    } else {
        size_t sz = scalar_size(t);
        if (!sz || !need(c, sz)) { c->bad = 1; return; }
        c->p += sz;
    }
}

int gguf_open(const char *path, gguf_file *f, char *err, size_t err_len) {
    memset(f, 0, sizeof(*f));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, err_len, "cannot open %s", path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 24) {
        close(fd);
        snprintf(err, err_len, "%s is not a GGUF file", path);
        return -1;
    }
    f->size = (size_t) st.st_size;
    f->map  = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f->map == MAP_FAILED) {
        f->map = NULL;
        snprintf(err, err_len, "mmap failed for %s", path);
        return -1;
    }

    cursor c = { f->map, (const uint8_t *) f->map + f->size, 0 };
    if (memcmp(c.p, "GGUF", 4) != 0) {
        snprintf(err, err_len, "%s: bad magic (not GGUF)", path);
        gguf_close(f);
        return -1;
    }
    c.p += 4;
    f->version = rd_u32(&c);
    if (f->version < 2 || f->version > 3) {
        snprintf(err, err_len, "%s: unsupported GGUF version %u", path, f->version);
        gguf_close(f);
        return -1;
    }
    f->n_tensors = rd_u64(&c);
    f->n_kv      = rd_u64(&c);
    if (f->n_kv > 1u << 20 || f->n_tensors > 1u << 24) {
        snprintf(err, err_len, "%s: implausible header counts", path);
        gguf_close(f);
        return -1;
    }

    f->kv      = calloc(f->n_kv ? f->n_kv : 1, sizeof(gguf_kv));
    f->tensors = calloc(f->n_tensors ? f->n_tensors : 1, sizeof(gguf_tensor));
    if (!f->kv || !f->tensors) {
        snprintf(err, err_len, "out of memory");
        gguf_close(f);
        return -1;
    }

    uint64_t alignment = 32;
    for (uint64_t i = 0; i < f->n_kv && !c.bad; i++) {
        gguf_kv *kv = &f->kv[i];
        kv->key  = rd_str(&c);
        kv->type = rd_u32(&c);
        if (kv->type == GGUF_ARRAY) {
            const uint8_t *start = c.p;
            kv->arr_type = rd_u32(&c);
            kv->arr_n    = rd_u64(&c);
            kv->value    = c.p;
            c.p          = start;
        } else {
            kv->value = c.p;
        }
        skip_value(&c, kv->type);
        if (kv->key.n == 17 && memcmp(kv->key.s, "general.alignment", 17) == 0 &&
            kv->type == GGUF_UINT32) {
            uint32_t a;
            memcpy(&a, kv->value, 4);
            alignment = a ? a : 32;
        }
    }

    for (uint64_t i = 0; i < f->n_tensors && !c.bad; i++) {
        gguf_tensor *t = &f->tensors[i];
        t->name   = rd_str(&c);
        t->n_dims = rd_u32(&c);
        if (t->n_dims > 4) { c.bad = 1; break; }
        for (int d = 0; d < 4; d++) t->ne[d] = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) t->ne[d] = (int64_t) rd_u64(&c);
        t->type   = rd_u32(&c);
        t->offset = rd_u64(&c);
    }
    if (c.bad) {
        snprintf(err, err_len, "%s: truncated or corrupt header", path);
        gguf_close(f);
        return -1;
    }

    size_t header = (size_t) (c.p - (const uint8_t *) f->map);
    size_t data   = (header + alignment - 1) / alignment * alignment;
    for (uint64_t i = 0; i < f->n_tensors; i++) {
        gguf_tensor *t   = &f->tensors[i];
        size_t       n   = (size_t) (t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3]);
        size_t       esz = t->type == GGML_TYPE_F32 ? 4 : t->type == GGML_TYPE_F16 ? 2 : 0;
        if (esz && data + t->offset + n * esz > f->size) {
            snprintf(err, err_len, "%s: tensor %.*s extends past end of file", path,
                     (int) t->name.n, t->name.s);
            gguf_close(f);
            return -1;
        }
        t->data = (const uint8_t *) f->map + data + t->offset;
    }
    return 0;
}

void gguf_close(gguf_file *f) {
    if (f->map) munmap(f->map, f->size);
    free(f->kv);
    free(f->tensors);
    memset(f, 0, sizeof(*f));
}

static int str_eq(gguf_str s, const char *z) {
    size_t n = strlen(z);
    return s.n == n && memcmp(s.s, z, n) == 0;
}

const gguf_kv *gguf_find_kv(const gguf_file *f, const char *key) {
    for (uint64_t i = 0; i < f->n_kv; i++)
        if (str_eq(f->kv[i].key, key)) return &f->kv[i];
    return NULL;
}

const gguf_tensor *gguf_find_tensor(const gguf_file *f, const char *name) {
    for (uint64_t i = 0; i < f->n_tensors; i++)
        if (str_eq(f->tensors[i].name, name)) return &f->tensors[i];
    return NULL;
}

static int read_int(uint32_t t, const uint8_t *p, int64_t *out) {
    switch (t) {
        case GGUF_UINT8:  *out = *(const uint8_t *) p; return 1;
        case GGUF_INT8:   *out = *(const int8_t *) p; return 1;
        case GGUF_BOOL:   *out = *p != 0; return 1;
        case GGUF_UINT16: { uint16_t v; memcpy(&v, p, 2); *out = v; return 1; }
        case GGUF_INT16:  { int16_t v;  memcpy(&v, p, 2); *out = v; return 1; }
        case GGUF_UINT32: { uint32_t v; memcpy(&v, p, 4); *out = v; return 1; }
        case GGUF_INT32:  { int32_t v;  memcpy(&v, p, 4); *out = v; return 1; }
        case GGUF_UINT64: { uint64_t v; memcpy(&v, p, 8); *out = (int64_t) v; return 1; }
        case GGUF_INT64:  { int64_t v;  memcpy(&v, p, 8); *out = v; return 1; }
        default: return 0;
    }
}

int gguf_get_int(const gguf_file *f, const char *key, int64_t *out) {
    const gguf_kv *kv = gguf_find_kv(f, key);
    return kv && kv->type != GGUF_ARRAY && read_int(kv->type, kv->value, out);
}

int gguf_get_float(const gguf_file *f, const char *key, double *out) {
    const gguf_kv *kv = gguf_find_kv(f, key);
    if (!kv) return 0;
    if (kv->type == GGUF_FLOAT32) { float v; memcpy(&v, kv->value, 4); *out = v; return 1; }
    if (kv->type == GGUF_FLOAT64) { memcpy(out, kv->value, 8); return 1; }
    int64_t i;
    if (read_int(kv->type, kv->value, &i)) { *out = (double) i; return 1; }
    return 0;
}

int gguf_get_bool(const gguf_file *f, const char *key, int *out) {
    int64_t v;
    if (!gguf_get_int(f, key, &v)) return 0;
    *out = v != 0;
    return 1;
}

int gguf_get_str(const gguf_file *f, const char *key, gguf_str *out) {
    const gguf_kv *kv = gguf_find_kv(f, key);
    if (!kv || kv->type != GGUF_STRING) return 0;
    memcpy(&out->n, kv->value, 8);   /* bounds were checked when the header was parsed */
    out->s = (const char *) kv->value + 8;
    return 1;
}

int64_t gguf_arr_int(const gguf_kv *kv, uint64_t i) {
    int64_t v = 0;
    size_t  sz = scalar_size(kv->arr_type);
    if (i < kv->arr_n && sz) read_int(kv->arr_type, kv->value + i * sz, &v);
    return v;
}

gguf_str gguf_arr_next_str(const gguf_kv *kv, const uint8_t **cur) {
    gguf_str s;
    const uint8_t *p = *cur ? *cur : kv->value;
    memcpy(&s.n, p, 8);
    s.s  = (const char *) p + 8;
    *cur = p + 8 + s.n;
    return s;
}
