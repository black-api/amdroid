/* klib.c — extra string functions the kernel-side Fuss interpreter needs. */
#include "amdroid/types.h"
#include "amdroid/heap.h"
#include <string.h>

char *strchr(const char *s, int c){
    for(;;s++){
        if(*s == (char)c) return (char*)s;
        if(!*s) return NULL;
    }
}

char *strrchr(const char *s, int c){
    const char *r = NULL;
    for(;;s++){
        if(*s == (char)c) r = s;
        if(!*s) return (char*)r;
    }
}

char *strstr(const char *h, const char *n){
    if(!*n) return (char*)h;
    for(; *h; h++){
        const char *a = h, *b = n;
        while(*a && *b && *a == *b){ a++; b++; }
        if(!*b) return (char*)h;
    }
    return NULL;
}

char *strdup(const char *s){
    size_t n = strlen(s);
    char *d = kmalloc(n + 1);
    if(!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

char *strndup(const char *s, size_t n){
    size_t l = 0;
    while(l < n && s[l]) l++;
    char *d = kmalloc(l + 1);
    if(!d) return NULL;
    memcpy(d, s, l);
    d[l] = 0;
    return d;
}

void *memmove(void *d, const void *s, size_t n){
    if((unsigned char*)d <= (const unsigned char*)s || (unsigned char*)d >= (const unsigned char*)s + n)
        return memcpy(d, s, n);              /* a forward copy is safe */
    unsigned char *dd = (unsigned char*)d + n - 1;
    const unsigned char *ss = (const unsigned char*)s + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) : : "memory");
    return d;
}
