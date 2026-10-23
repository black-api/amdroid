#ifndef AMDROID_STRING_H
#define AMDROID_STRING_H
#include "amdroid/types.h"
size_t strlen(const char *s);
int strcmp(const char *a,const char *b);
int strncmp(const char *a,const char *b,size_t n);
char *strcpy(char *d,const char *s);
char *strcat(char *d,const char *s);
void *memcpy(void *d,const void *s,size_t n);
void *memset(void *d,int v,size_t n);
int memcmp(const void *a,const void *b,size_t n);
#endif
