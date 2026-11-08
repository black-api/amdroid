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
    unsigned char *dd = d;
    const unsigned char *ss = s;
    if(dd < ss){
        for(size_t i = 0; i < n; i++) dd[i] = ss[i];
    } else {
        for(size_t i = n; i > 0; i--) dd[i-1] = ss[i-1];
    }
    return d;
}
