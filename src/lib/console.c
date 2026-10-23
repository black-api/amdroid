#include "amdroid/console.h"
#include "amdroid/vga.h"
#include "amdroid/serial.h"
#include "amdroid/vga.h"
#include "amdroid/serial.h"
#include "amdroid/types.h"
#include <stdarg.h>

void console_init(void){ serial_init(); vga_init(); }

static void console_emit(char c){
    serial_putc(c);
    vga_putc(c);
}

void console_putc(char c){ console_emit(c); }
void console_puts(const char *s){ while(*s) console_emit(*s++); }

static void utoa(char *buf, unsigned long long v, int base, int upper){
    const char *digits = upper?"0123456789ABCDEF":"0123456789abcdef";
    char tmp[64]; int n=0;
    if(v==0) tmp[n++]='0';
    while(v){ tmp[n++]=digits[v%base]; v/=base; }
    for(int i=0;i<n;i++) buf[i]=tmp[n-1-i];
    buf[n]=0;
}

void console_printf(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    char buf[128];
    while(*fmt){
        if(*fmt!='%'){ console_emit(*fmt++); continue; }
        fmt++;
        int longflag=0;
        if(*fmt=='l'){ longflag=1; fmt++; if(*fmt=='l'){ longflag=2; fmt++; } }
        switch(*fmt){
            case 'd': case 'i': {
                long long v = longflag? va_arg(ap,long long):va_arg(ap,int);
                if(v<0){ console_emit('-'); v=-v; }
                utoa(buf,(unsigned long long)v,10,0);
                console_puts(buf); break;
            }
            case 'u': {
                unsigned long long v = longflag? va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
                utoa(buf,v,10,0); console_puts(buf); break;
            }
            case 'x': {
                unsigned long long v = longflag? va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
                utoa(buf,v,16,0); console_puts(buf); break;
            }
            case 'X': {
                unsigned long long v = longflag? va_arg(ap,unsigned long long):va_arg(ap,unsigned int);
                utoa(buf,v,16,1); console_puts(buf); break;
            }
            case 'p': {
                void *p = va_arg(ap,void*);
                console_puts("0x");
                utoa(buf,(unsigned long long)p,16,0); console_puts(buf); break;
            }
            case 's': {
                const char *s = va_arg(ap,const char*);
                if(!s) s="(null)";
                console_puts(s); break;
            }
            case 'c': {
                char c = (char)va_arg(ap,int);
                console_emit(c); break;
            }
            case '%': console_emit('%'); break;
            default: console_emit('%'); console_emit(*fmt); break;
        }
        fmt++;
    }
    va_end(ap);
}
