/* Minimal read-only GGUF (v2/v3) parser over an mmap'd file. */
#ifndef THIMBLE_GGUF_H
#define THIMBLE_GGUF_H

#include <stddef.h>
#include <stdint.h>

enum gguf_type {
    GGUF_UINT8 = 0, GGUF_INT8 = 1, GGUF_UINT16 = 2, GGUF_INT16 = 3, GGUF_UINT32 = 4,
    GGUF_INT32 = 5, GGUF_FLOAT32 = 6, GGUF_BOOL = 7, GGUF_STRING = 8, GGUF_ARRAY = 9,
    GGUF_UINT64 = 10, GGUF_INT64 = 11, GGUF_FLOAT64 = 12,
};

enum { GGML_TYPE_F32 = 0, GGML_TYPE_F16 = 1 };

typedef struct {
    const char *s;
    uint64_t    n;
} gguf_str;

typedef struct {
    gguf_str       key;
    uint32_t       type;
    const uint8_t *value;     /* raw value (for arrays: first item) */
    uint32_t       arr_type;  /* GGUF_ARRAY only */
    uint64_t       arr_n;
} gguf_kv;

typedef struct {
    gguf_str     name;
    uint32_t     n_dims;
    int64_t      ne[4];       /* ggml order: ne[0] is the fastest-moving dim */
    uint32_t     type;
    uint64_t     offset;
    const void  *data;
} gguf_tensor;

typedef struct {
    void        *map;
    size_t       size;
    uint32_t     version;
    uint64_t     n_kv;
    uint64_t     n_tensors;
    gguf_kv     *kv;
    gguf_tensor *tensors;
} gguf_file;

int  gguf_open(const char *path, gguf_file *f, char *err, size_t err_len);
void gguf_close(gguf_file *f);

const gguf_kv     *gguf_find_kv(const gguf_file *f, const char *key);
const gguf_tensor *gguf_find_tensor(const gguf_file *f, const char *name);

/* Typed getters; return 1 if found and convertible. */
int gguf_get_int(const gguf_file *f, const char *key, int64_t *out);
int gguf_get_float(const gguf_file *f, const char *key, double *out);
int gguf_get_bool(const gguf_file *f, const char *key, int *out);
int gguf_get_str(const gguf_file *f, const char *key, gguf_str *out);

/* Array access: item i of an int array (any int type). */
int64_t gguf_arr_int(const gguf_kv *kv, uint64_t i);
/* Iterate a string array: pass *cursor = NULL first. */
gguf_str gguf_arr_next_str(const gguf_kv *kv, const uint8_t **cursor);

#endif
