/* run.c — entry point, error reporting, and file access for the Fuss interpreter.
 * Errors and 'exit' unwind with __builtin_setjmp/__builtin_longjmp, since the
 * kernel has no libc setjmp. */
#include "fuss_int.h"
#include "amdroid/heap.h"
#include "amdroid/console.h"
#include "amdroid/fat32.h"
#include "amdroid/fuss.h"
#include "amdroid/auth.h"
#include <stdarg.h>
#include <string.h>

const char *fz_file;
int fz_line;
int fz_error_count;

static void *jb[5];
static int run_status;

void fz_finish(int code){
    run_status = code;
    __builtin_longjmp(jb, 1);
    __builtin_unreachable();
}

/* minimal printf: %s %d %u %x %c %% */
static void kvformat(char *out, int cap, const char *fmt, va_list ap){
    int n = 0;
    #define PUT(ch) do{ if(n < cap - 1) out[n] = (ch); n++; }while(0)
    for(const char *p = fmt; *p; p++){
        if(*p != '%'){ PUT(*p); continue; }
        p++;
        if(*p == 's'){
            const char *s = va_arg(ap, const char*);
            if(!s) s = "(null)";
            for(; *s; s++) PUT(*s);
        } else if(*p == 'd' || *p == 'u' || *p == 'x'){
            unsigned long long v;
            int neg = 0;
            if(*p == 'd'){
                long long sv = va_arg(ap, int);
                if(sv < 0){ neg = 1; v = (unsigned long long)(-sv); }
                else v = (unsigned long long)sv;
            } else {
                v = va_arg(ap, unsigned int);
            }
            int base = *p == 'x' ? 16 : 10;
            char dg[24]; int k = 0;
            do { dg[k++] = "0123456789abcdef"[v % base]; v /= base; } while(v);
            if(neg) PUT('-');
            while(k) PUT(dg[--k]);
        } else if(*p == 'c'){
            PUT((char)va_arg(ap, int));
        } else if(*p == '%'){
            PUT('%');
        } else {
            PUT('%');
            if(*p) PUT(*p);
        }
    }
    if(cap > 0) out[n < cap - 1 ? n : cap - 1] = 0;
    #undef PUT
}

void fz_report(const char *file, int line, const char *fmt, ...){
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    kvformat(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if(file && line > 0) console_printf("fuss: %s:%d: %s\n", file, line, msg);
    else console_printf("fuss: %s\n", msg);
    fz_error_count++;
}

void fz_die(const char *fmt, ...){
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    kvformat(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if(fz_file && fz_line > 0)
        console_printf("fuss: %s:%d: %s\n", fz_file, fz_line, msg);
    else
        console_printf("fuss: %s\n", msg);
    fz_finish(1);
}

/* Reads a file (root or subdirectory path). Returns 0 and a NUL-terminated buffer. */
int fz_read_fat(const char *name, char **text, u32 *len){
    char canon[80];
    if(!fz_canon_path(name, canon, sizeof canon)) return -1;
    if(!auth_may(canon, 0)) return -1;   /* the caller may not read this file */
    u32 size = 0;
    if(fat32_read_path(canon, NULL, 0, &size) != 0) return -1;   /* size query */
    char *buf = kmalloc(size + 1);
    if(!buf) fz_die("out of memory");
    u32 got = 0;
    if(fat32_read_path(canon, buf, size, &got) != 0) return -1;
    buf[got] = 0;
    *text = buf;
    if(len) *len = got;
    return 0;
}

/* Runs a program from the FAT32 root. Output goes to the console. */
int fuss_run_file(const char *name){
    fz_reset();
    fz_file = NULL;
    fz_line = 0;
    fz_error_count = 0;
    run_status = 0;
    if(__builtin_setjmp(jb)){
        return run_status;
    }
    char *text; u32 len;
    if(fz_read_fat(name, &text, &len) != 0){
        console_printf("fuss: cannot open '%s' (FAT32 root, 8.3 name)\n", name);
        return 1;
    }
    fz_compile_main(name, text);
    fz_resolve();
    if(fz_error_count){
        console_printf("fuss: %d error(s) found\n", fz_error_count);
        return 1;
    }
    fz_glob = kcalloc((size_t)(fz_nglobals ? fz_nglobals : 1), sizeof(Val));
    if(!fz_glob) fz_die("out of memory");
    fz_run_program();
    return 0;
}
