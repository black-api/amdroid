#include "amdroid/types.h"

size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char *a,const char *b){ while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a,const char *b,size_t n){ for(size_t i=0;i<n;i++){ if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i]; if(!a[i]) return 0;} return 0; }
char *strcpy(char *d,const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strcat(char *d,const char *s){ char *r=d; while(*d) d++; while((*d++=*s++)); return r; }
/* Eight bytes at a time, then the tail: screens and pages are copied with this. */
void *memcpy(void *d,const void *s,size_t n){
    void *r=d; size_t q=n>>3, t=n&7;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(t) : : "memory");
    return r;
}
void *memset(void *d,int v,size_t n){
    void *r=d; size_t q=n>>3, t=n&7;
    unsigned long long pat=0x0101010101010101ULL*(unsigned char)v;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(pat) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(t) : "a"(pat) : "memory");
    return r;
}
int memcmp(const void *a,const void *b,size_t n){ const unsigned char *aa=a,*bb=b; for(size_t i=0;i<n;i++){ if(aa[i]!=bb[i]) return aa[i]-bb[i]; } return 0; }
