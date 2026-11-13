/* jpegbench.c — times the OS's JPEG decoder on frames (host build). Usage: jpegbench f*.jpg */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "amdroid/types.h"
int jpeg_decode_rgb(const u8 *data, u32 len, u32 **px, u32 *w, u32 *h);
void kfree(void *p);
int main(int argc, char **argv){
    struct timespec a, b; double total = 0; int n = 0;
    for(int r = 0; r < 3; r++) for(int i = 1; i < argc; i++){
        FILE *f = fopen(argv[i], "rb"); if(!f) continue;
        fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
        u8 *d = malloc(L); fread(d, 1, L, f); fclose(f);
        u32 *px; u32 w, h;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int rc = jpeg_decode_rgb(d, (u32)L, &px, &w, &h);
        clock_gettime(CLOCK_MONOTONIC, &b);
        if(rc == 0){ total += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; n++; }
        if(r == 0 && i == 1){
            printf("%ux%u\n", w, h);
            if(getenv("PPM")){ FILE *o = fopen(getenv("PPM"), "wb"); fprintf(o, "P6\n%u %u\n255\n", w, h);
              for(u32 k = 0; k < w * h; k++){ unsigned char c[3] = { px[k] >> 16, px[k] >> 8, px[k] }; fwrite(c, 1, 3, o); } fclose(o); }
        }
        if(rc == 0) kfree(px);
        free(d);
    }
    printf("%d frames, %.2f ms per frame (native)\n", n, total / n);
    return 0;
}
