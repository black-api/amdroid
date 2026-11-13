/* jsrun.c — run a .js file through Am-Droid's JS engine on the host (no QEMU).
 * Usage: jsrun file.js     prints console.log output, then OK or the error with its
 * line:column and the source around it. */
#include <stdio.h>
#include <stdlib.h>
#include "amdroid/types.h"
#include "amdroid/js.h"

char stack_top[1];
void js_set_stack_top(unsigned long t);
extern int js_parse_only;
extern int js_gc_stress;
u32 js_heap_used(void);
void js_collect(void);

int main(int argc, char **argv){
    js_set_stack_top((unsigned long)__builtin_frame_address(0) + 4096);
    while(argc > 2 && argv[1][0] == '-'){
        if(argv[1][1] == 'p') js_parse_only = 1;            /* parse only (bodies lazily) */
        else if(argv[1][1] == 'P') js_parse_only = 2;       /* parse only, every body */
        else if(argv[1][1] == 'g'){ js_gc_stress = atoi(argv[2]); argv++; argc--; }
        else if(argv[1][1] == 'm'){ js_set_heap_size((u32)atoi(argv[2]) << 20); argv++; argc--; }
        argv++; argc--;
    }
    if(argc < 2){ fprintf(stderr, "usage: jsrun [-p|-P] [-m MB] file.js\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if(!f){ perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc(n + 1); n = fread(src, 1, n, f); src[n] = 0; fclose(f);
    js_init();
    int r = js_run(src, (u32)n);
    if(r == 0 && !js_parse_only) for(int t = 0; t < 20; t++) if(!js_timers_tick(1000000000ull + t)) break;
    js_collect();
    fprintf(stderr, "heap in use: %.1f MB\n", js_heap_used() / 1048576.0);
    if(r == 0){ const char *l = js_last_log(); if(l && *l) printf("log: %s\n", l); printf("OK\n"); return 0; }
    u32 at = js_error_at();
    printf("ERROR: %s\n", js_error());
    if(at != 0xFFFFFFFFu && at <= (u32)n){
        int line = 1, col = 1;
        for(u32 i = 0; i < at; i++){ if(src[i] == '\n'){ line++; col = 1; } else col++; }
        printf("  at %u (line %d, col %d)\n  ", at, line, col);
        long a = (long)at - 60, b = (long)at + 60;
        if(a < 0) a = 0; if(b > n) b = n;
        for(long i = a; i < b; i++){ char c = src[i]; putchar(c == '\n' ? ' ' : c); }
        printf("\n  ");
        for(long i = a; i < (long)at; i++) putchar(' ');
        printf("^\n");
    }
    return 1;
}
