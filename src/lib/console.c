#include "amdroid/console.h"
#include "amdroid/vga.h"
#include "amdroid/serial.h"
#include "amdroid/fbcon.h"
#include "amdroid/types.h"
#include <stdarg.h>

static int strlen_k(const char *s){ int n = 0; while(s[n]) n++; return n; }

void console_init(void){ serial_init(); vga_init(); }

static void console_emit(char c){
    serial_putc(c);
    vga_putc(c);
    fbcon_putc(c);
}

void console_putc(char c){ console_emit(c); }
void console_puts(const char *s){ while(*s) console_emit(*s++); fbcon_flush(0); }

static void utoa(char *buf, unsigned long long v, int base, int upper){
    const char *digits = upper?"0123456789ABCDEF":"0123456789abcdef";
    char tmp[64]; int n=0;
    if(v==0) tmp[n++]='0';
    while(v){ tmp[n++]=digits[v%base]; v/=base; }
    for(int i=0;i<n;i++) buf[i]=tmp[n-1-i];
    buf[n]=0;
}

/* Emits a field: pads to width (left-justified if left, zero-filled if zero). */
static void emit_field(const char *body, int len, int width, int left, int zero){
    int pad = width > len ? width - len : 0;
    if(!left) for(int i = 0; i < pad; i++) console_emit(zero ? '0' : ' ');
    for(int i = 0; i < len; i++) console_emit(body[i]);
    if(left) for(int i = 0; i < pad; i++) console_emit(' ');
}

void console_printf(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    char buf[128];
    while(*fmt){
        if(*fmt!='%'){ console_emit(*fmt++); continue; }
        fmt++;
        int left = 0, zero = 0, width = 0, longflag = 0;
        for(;;){
            if(*fmt == '-'){ left = 1; fmt++; }
            else if(*fmt == '0' && !width){ zero = 1; fmt++; }
            else break;
        }
        while(*fmt >= '0' && *fmt <= '9'){ width = width * 10 + (*fmt - '0'); fmt++; }
        if(*fmt=='l'){ longflag=1; fmt++; if(*fmt=='l'){ longflag=2; fmt++; } }
        switch(*fmt){
            case 'd': case 'i': {
                long long v = longflag? va_arg(ap,long long):va_arg(ap,int);
                int neg = v < 0;
                unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
                char tmp[24]; int n = 0;
                utoa(tmp, u, 10, 0); n = (int)strlen_k(tmp);
                char out[26]; int k = 0;
                if(neg) out[k++] = '-';
                for(int i = 0; i < n; i++) out[k++] = tmp[i];
                emit_field(out, k, width, left, zero);
                break;
            }
            case 'u': case 'x': case 'X': {
                unsigned long long v = longflag? va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
                utoa(buf, v, *fmt == 'u' ? 10 : 16, *fmt == 'X');
                emit_field(buf, (int)strlen_k(buf), width, left, zero);
                break;
            }
            case 'p': {
                void *p = va_arg(ap,void*);
                utoa(buf,(unsigned long long)p,16,0);
                int n = (int)strlen_k(buf);
                char out[20]; int k = 0;
                out[k++]='0'; out[k++]='x';
                for(int i = 0; i < n; i++) out[k++] = buf[i];
                emit_field(out, k, width, left, 0);
                break;
            }
            case 's': {
                const char *str = va_arg(ap,const char*);
                if(!str) str="(null)";
                emit_field(str, (int)strlen_k(str), width, left, 0);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap,int);
                emit_field(&c, 1, width, left, 0);
                break;
            }
            case '%': console_emit('%'); break;
            default: console_emit('%'); console_emit(*fmt); break;
        }
        fmt++;
    }
    va_end(ap);
    fbcon_flush(0);
}
