#include "amdroid/types.h"

size_t strlen(const char *s){ size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char *a,const char *b){ while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a,const char *b,size_t n){ for(size_t i=0;i<n;i++){ if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i]; if(!a[i]) return 0;} return 0; }
char *strcpy(char *d,const char *s){ char *r=d; while((*d++=*s++)); return r; }
char *strcat(char *d,const char *s){ char *r=d; while(*d) d++; while((*d++=*s++)); return r; }
void *memcpy(void *d,const void *s,size_t n){ unsigned char *dd=d; const unsigned char *ss=s; for(size_t i=0;i<n;i++) dd[i]=ss[i]; return d; }
void *memset(void *d,int v,size_t n){ unsigned char *dd=d; for(size_t i=0;i<n;i++) dd[i]=v; return d; }
int memcmp(const void *a,const void *b,size_t n){ const unsigned char *aa=a,*bb=b; for(size_t i=0;i<n;i++){ if(aa[i]!=bb[i]) return aa[i]-bb[i]; } return 0; }
char *strtok(char *str,const char *delim){ (void)str;(void)delim; return NULL; } /* minimal */
