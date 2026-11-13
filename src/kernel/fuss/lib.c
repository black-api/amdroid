/* lib.c — Fuss standard library plus am-droid specific builtins.
 * Writes are not supported yet: the FAT32 driver is read-only. */
#include "fuss_int.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/serial.h"
#include "amdroid/keyboard.h"
#include "amdroid/fat32.h"
#include "amdroid/fb.h"
#include "amdroid/mouse.h"
#include "amdroid/timer.h"
#include "amdroid/kmath.h"
#include "amdroid/sched.h"
#include "amdroid/auth.h"
#include <string.h>

typedef Val (*bfn_t)(Val *a, int n);
typedef struct { const char *name; int argc; bfn_t fn; } BDef;

#define BI(name) static Val bi_##name(Val *a, int n)
#define UNUSED_ARGS (void)a; (void)n

static u64 rng_state;
static int rng_seeded;

static u64 rng_next(void){
    if(!rng_seeded){ rng_state = timer_ticks() * 2654435761ULL + 1; rng_seeded = 1; }
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng_state >> 33;   /* 31 bits */
}

static void need_str(Val v, const char *what){
    if(v.t != V_STR) fz_die("%s expects a string", what);
}

/* ----- core ----- */
BI(len){
    UNUSED_ARGS;
    switch(a[0].t){
        case V_ARR: return fz_num(a[0].a->len);
        case V_STR: return fz_num((double)strlen(a[0].s));
        case V_STRUCT: return fz_num(a[0].o->n);
        default: fz_die("len() expects an array, string or struct");
    }
    return fz_num(0);
}
BI(push){
    UNUSED_ARGS;
    if(a[0].t != V_ARR) fz_die("push() expects an array");
    fz_arr_push(a[0].a, a[1]);
    return a[0];
}
BI(pop){
    UNUSED_ARGS;
    if(a[0].t != V_ARR) fz_die("pop() expects an array");
    if(a[0].a->len == 0) fz_die("pop() from an empty array");
    return a[0].a->items[--a[0].a->len];
}
BI(type){
    UNUSED_ARGS;
    switch(a[0].t){
        case V_NUM: return fz_strv("num");
        case V_STR: return fz_strv("str");
        case V_BOOL: return fz_strv("bool");
        case V_ARR: return fz_strv("arr");
        default: return fz_strv("struct");
    }
}

/* ----- math ----- */
BI(abs){ UNUSED_ARGS; return fz_num(kfabs(fz_numof(a[0]))); }
BI(min){ UNUSED_ARGS; double x = fz_numof(a[0]), y = fz_numof(a[1]); return fz_num(x < y ? x : y); }
BI(max){ UNUSED_ARGS; double x = fz_numof(a[0]), y = fz_numof(a[1]); return fz_num(x > y ? x : y); }
BI(floor){ UNUSED_ARGS; return fz_num(kfloor(fz_numof(a[0]))); }
BI(ceil){ UNUSED_ARGS; return fz_num(kceil(fz_numof(a[0]))); }
BI(sqrt){
    UNUSED_ARGS;
    double d = fz_numof(a[0]);
    if(d < 0) fz_die("sqrt of a negative number");
    return fz_num(ksqrt(d));
}
BI(pow){ UNUSED_ARGS; return fz_num(kpow(fz_numof(a[0]), fz_numof(a[1]))); }
BI(sin){ UNUSED_ARGS; return fz_num(ksin(fz_numof(a[0]))); }
BI(cos){ UNUSED_ARGS; return fz_num(kcos(fz_numof(a[0]))); }
BI(tan){ UNUSED_ARGS; return fz_num(ktan(fz_numof(a[0]))); }
BI(atan2){ UNUSED_ARGS; return fz_num(katan2(fz_numof(a[0]), fz_numof(a[1]))); }

/* ----- conversions ----- */
BI(num){ UNUSED_ARGS; return fz_num(fz_numof(a[0])); }
BI(str){ UNUSED_ARGS; return fz_strv(fz_tostr(a[0])); }
BI(bool){ UNUSED_ARGS; return fz_bool(fz_truthy(a[0])); }

/* ----- random ----- */
BI(rand){ UNUSED_ARGS; return fz_num((double)rng_next()); }
BI(rand_seed){
    UNUSED_ARGS;
    rng_state = (u64)(long long)fz_numof(a[0]);
    rng_seeded = 1;
    return fz_num(0);
}
BI(randbelow){
    UNUSED_ARGS;
    double d = fz_numof(a[0]);
    if(d <= 0) fz_die("randbelow needs a positive number");
    return fz_num((double)(rng_next() % (unsigned long long)d));
}

/* ----- strings ----- */
BI(upper){
    UNUSED_ARGS;
    need_str(a[0], "upper()");
    char *r = fz_strdup(a[0].s);
    for(char *p = r; *p; p++) if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    return fz_strv(r);
}
BI(lower){
    UNUSED_ARGS;
    need_str(a[0], "lower()");
    char *r = fz_strdup(a[0].s);
    for(char *p = r; *p; p++) if(*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    return fz_strv(r);
}
BI(contains){
    UNUSED_ARGS;
    need_str(a[0], "contains()");
    need_str(a[1], "contains()");
    return fz_bool(strstr(a[0].s, a[1].s) != NULL);
}
BI(substr){
    if(n != 2 && n != 3) fz_die("substr takes 2 or 3 arguments");
    need_str(a[0], "substr()");
    int len = (int)strlen(a[0].s);
    int start = (int)fz_numof(a[1]);
    if(start < 0) start = 0;
    if(start > len) start = len;
    int count = n == 3 ? (int)fz_numof(a[2]) : len - start;
    if(count < 0) count = 0;
    if(count > len - start) count = len - start;
    return fz_strv(fz_strndup(a[0].s + start, (size_t)count));
}
BI(split){
    UNUSED_ARGS;
    need_str(a[0], "split()");
    need_str(a[1], "split()");
    size_t dl = strlen(a[1].s);
    if(dl == 0) fz_die("split(): empty separator");
    Arr *out = fz_arr_new();
    const char *p = a[0].s, *q;
    while((q = strstr(p, a[1].s)) != NULL){
        fz_arr_push(out, fz_strv(fz_strndup(p, (size_t)(q - p))));
        p = q + dl;
    }
    fz_arr_push(out, fz_strv(fz_strdup(p)));
    return fz_arrv(out);
}
BI(join){
    UNUSED_ARGS;
    if(a[0].t != V_ARR) fz_die("join() expects an array");
    char *sep = fz_tostr(a[1]);
    Sb sb = { NULL, 0, 0 };
    for(int i = 0; i < a[0].a->len; i++){
        if(i) fz_sb_adds(&sb, sep);
        fz_emit(&sb, a[0].a->items[i], 0);
    }
    return fz_strv(fz_sb_done(&sb));
}
BI(char_code_at){
    UNUSED_ARGS;
    need_str(a[0], "char_code_at()");
    int idx = (int)fz_numof(a[1]);
    if(idx < 0 || idx >= (int)strlen(a[0].s)) fz_die("char_code_at index out of bounds");
    return fz_num((unsigned char)a[0].s[idx]);
}
BI(range){
    UNUSED_ARGS;
    double lo = kfloor(fz_numof(a[0])), hi = kfloor(fz_numof(a[1]));
    Arr *out = fz_arr_new();
    for(double i = lo; i < hi; i += 1) fz_arr_push(out, fz_num(i));
    return fz_arrv(out);
}

/* ----- input, time, environment ----- */
static int getkey(void){
    for(;;){
        if(serial_has_input()) return serial_getc();
        if(keyboard_has_input()) return keyboard_getchar();
        proc_yield();
    }
}
BI(input){
    UNUSED_ARGS;
    Sb sb = { NULL, 0, 0 };
    for(;;){
        int c = getkey();
        if(c == '\r' || c == '\n'){ console_putc('\n'); break; }
        if(c == '\b'){
            if(sb.len > 0){ sb.len--; sb.b[sb.len] = 0; console_puts("\b \b"); }
            continue;
        }
        if(c >= 32 && c < 127){ fz_sb_addc(&sb, (char)c); console_putc((char)c); }
    }
    return fz_strv(fz_sb_done(&sb));
}
BI(ticks){ UNUSED_ARGS; return fz_num((double)timer_ticks()); }
BI(time){ UNUSED_ARGS; return fz_num((double)(timer_ticks() / 100)); }   /* seconds since boot */
BI(sleep_ms){ UNUSED_ARGS; timer_sleep((u64)fz_numof(a[0])); return fz_num(0); }
BI(env){ UNUSED_ARGS; return fz_strv(""); }

/* ----- files (FAT32 root, read-only) ----- */
BI(read_file){
    UNUSED_ARGS;
    need_str(a[0], "read_file()");
    char *text; u32 len;
    if(fz_read_fat(a[0].s, &text, &len) != 0) fz_die("cannot read file '%s'", a[0].s);
    char *copy = fz_strdup(text);
    kfree(text);
    return fz_strv(copy);
}

BI(file_exists){
    UNUSED_ARGS;
    need_str(a[0], "file_exists()");
    char canon[80];
    if(!fz_canon_path(a[0].s, canon, sizeof canon)) return fz_bool(0);
    u32 size = 0;
    return fz_bool(fat32_read_path(canon, NULL, 0, &size) == 0);
}

static Arr *files_acc;
static void files_cb(const char *name, int is_dir, u32 size){
    (void)size;
    if(!is_dir) fz_arr_push(files_acc, fz_strv(fz_strdup(name)));
}
BI(files){
    UNUSED_ARGS;
    Arr *acc = fz_arr_new();      /* kept on the C stack: it is a GC root while listing */
    files_acc = acc;
    fat32_list_root(files_cb);
    return fz_arrv(acc);
}
BI(write_file){
    UNUSED_ARGS;
    need_str(a[0], "write_file()");
    char canon[16];
    if(!fz_canon_path(a[0].s, canon, sizeof canon))
        fz_die("write_file: '%s' must be a valid path of 8.3 names ", a[0].s);
    if(!auth_may(canon, 1)) fz_die("write_file: permission denied for '%s'", canon);
    char *text = fz_tostr(a[1]);
    if(fat32_write_path(canon, text, (u32)strlen(text)) != 0)
        fz_die("write_file: cannot write '%s' (disk full or write error)", a[0].s);
    auth_claim(canon);
    return fz_bool(1);
}
BI(delete_file){
    UNUSED_ARGS;
    need_str(a[0], "delete_file()");
    char canon[16];
    if(!fz_canon_path(a[0].s, canon, sizeof canon)) return fz_bool(0);
    if(!auth_may(canon, 1)) return fz_bool(0);
    return fz_bool(fat32_delete_path(canon) == 0);
}
BI(mkdir_p){
    UNUSED_ARGS;
    fz_die("mkdir_p: subdirectories are not supported (only the disk root)");
    return fz_num(0);
}
BI(write_ppm){
    UNUSED_ARGS;
    need_str(a[0], "write_ppm()");
    char canon[16];
    if(!fz_canon_path(a[0].s, canon, sizeof canon))
        fz_die("write_ppm: name '%s' must be a valid path of 8.3 names", a[0].s);
    if(a[3].t != V_ARR) fz_die("write_ppm expects an array of 0xRRGGBB ints");
    int W = (int)fz_numof(a[1]), H = (int)fz_numof(a[2]);
    if(W < 0 || H < 0) fz_die("write_ppm: negative dimensions");
    if(a[3].a->len < W * H) fz_die("write_ppm: pixels array too small");
    char header[32];
    int hl = 0;
    {
        Sb hb = { NULL, 0, 0 };
        fz_sb_adds(&hb, "P6\n");
        char num[12];
        int k = 0; unsigned v = (unsigned)W;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while(v);
        while(k) fz_sb_addc(&hb, num[--k]);
        fz_sb_addc(&hb, ' ');
        k = 0; v = (unsigned)H;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while(v);
        while(k) fz_sb_addc(&hb, num[--k]);
        fz_sb_adds(&hb, "\n255\n");
        hl = hb.len;
        if(hl > (int)sizeof header) fz_die("write_ppm: header too long");
        memcpy(header, hb.b, (size_t)hl);
    }
    u32 total = (u32)hl + (u32)W * (u32)H * 3;
    u8 *img = fz_alloc(total);
    memcpy(img, header, (size_t)hl);
    for(int i = 0; i < W * H; i++){
        u32 c = (u32)(long long)fz_numof(a[3].a->items[i]);
        u8 *px = img + hl + i * 3;
        px[0] = (u8)(c >> 16); px[1] = (u8)(c >> 8); px[2] = (u8)c;
    }
    if(!auth_may(canon, 1)) fz_die("write_ppm: permission denied for '%s'", canon);
    int rc = fat32_write_path(canon, img, total);
    if(rc != 0) fz_die("write_ppm: cannot write '%s' (disk full or write error)", a[0].s);
    auth_claim(canon);
    return fz_bool(1);
}

/* ----- framebuffer ----- */
static void need_fb(void){
    if(!fb_ready()) fz_die("no framebuffer (graphics mode not available)");
}
BI(fb_width){ UNUSED_ARGS; need_fb(); return fz_num(fb_width()); }
BI(fb_height){ UNUSED_ARGS; need_fb(); return fz_num(fb_height()); }
BI(fb_clear){
    UNUSED_ARGS; need_fb();
    fb_clear((u32)(long long)fz_numof(a[0]));
    return fz_num(0);
}
BI(fb_rect){
    UNUSED_ARGS; need_fb();
    fb_fill_rect((u32)fz_numof(a[0]), (u32)fz_numof(a[1]), (u32)fz_numof(a[2]),
                 (u32)fz_numof(a[3]), (u32)(long long)fz_numof(a[4]));
    return fz_num(0);
}
BI(fb_text){
    UNUSED_ARGS; need_fb();
    char *s = fz_tostr(a[2]);
    fb_draw_text((u32)fz_numof(a[0]), (u32)fz_numof(a[1]), s,
                 (u32)(long long)fz_numof(a[3]), (u32)(long long)fz_numof(a[4]));
    return fz_num(0);
}
BI(fb_present){ UNUSED_ARGS; need_fb(); fb_swap(); return fz_num(0); }
/* Non-blocking key read for full-screen apps: a key code, or -1 when none is waiting. */
BI(poll_key){
    UNUSED_ARGS;
    if(serial_has_input()) return fz_num((double)serial_getc());
    if(keyboard_has_input()) return fz_num((double)keyboard_getchar());
    proc_yield();
    return fz_num(-1);
}
/* Non-blocking mouse click poll: 1 if a button went down since the last call,
 * else 0. Lets full-screen apps offer a reliable mouse exit. */
BI(poll_click){
    UNUSED_ARGS;
    mouse_event_t ev;
    if(mouse_poll(&ev) && ev.pressed) return fz_num(1);
    return fz_num(0);
}

static const BDef BUILTINS[] = {
    { "len", 1, bi_len }, { "push", 2, bi_push }, { "pop", 1, bi_pop }, { "type", 1, bi_type },
    { "abs", 1, bi_abs }, { "min", 2, bi_min }, { "max", 2, bi_max },
    { "floor", 1, bi_floor }, { "ceil", 1, bi_ceil }, { "sqrt", 1, bi_sqrt },
    { "pow", 2, bi_pow }, { "sin", 1, bi_sin }, { "cos", 1, bi_cos }, { "tan", 1, bi_tan },
    { "atan2", 2, bi_atan2 },
    { "num", 1, bi_num }, { "str", 1, bi_str }, { "bool", 1, bi_bool },
    { "rand", 0, bi_rand }, { "rand_seed", 1, bi_rand_seed }, { "randbelow", 1, bi_randbelow },
    { "upper", 1, bi_upper }, { "lower", 1, bi_lower }, { "contains", 2, bi_contains },
    { "substr", -1, bi_substr }, { "split", 2, bi_split }, { "join", 2, bi_join },
    { "char_code_at", 2, bi_char_code_at }, { "range", 2, bi_range },
    { "input", 0, bi_input }, { "ticks", 0, bi_ticks }, { "time", 0, bi_time },
    { "sleep_ms", 1, bi_sleep_ms }, { "env", 1, bi_env },
    { "read_file", 1, bi_read_file }, { "file_exists", 1, bi_file_exists }, { "files", 0, bi_files },
    { "write_file", 2, bi_write_file }, { "delete_file", 1, bi_delete_file },
    { "mkdir_p", 1, bi_mkdir_p }, { "write_ppm", 4, bi_write_ppm },
    { "fb_width", 0, bi_fb_width }, { "fb_height", 0, bi_fb_height },
    { "fb_clear", 1, bi_fb_clear }, { "fb_rect", 5, bi_fb_rect },
    { "fb_text", 5, bi_fb_text }, { "fb_present", 0, bi_fb_present },
    { "poll_key", 0, bi_poll_key }, { "poll_click", 0, bi_poll_click },
};

int fz_builtin_find(const char *name){
    for(int i = 0; i < (int)(sizeof BUILTINS / sizeof BUILTINS[0]); i++)
        if(!strcmp(BUILTINS[i].name, name)) return i;
    return -1;
}

int fz_builtin_argc(int id){ return BUILTINS[id].argc; }

Val fz_builtin_call(int id, Val *args, int nargs){
    return BUILTINS[id].fn(args, nargs);
}
