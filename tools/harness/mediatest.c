/* mediatest.c — runs the OS's media pipeline (src/media) on the host, against real files.
 *
 *   mediatest FILE [-y out.yuv] [-a out.s16] [-p N out.ppm] [-c chunk] [-q]
 *
 * Appends FILE to a source buffer in pieces of `chunk` bytes (default: odd sizes, to prove
 * the parser copes with any split), lists the tracks and buffered ranges, then decodes every
 * frame. -y writes the decoded pictures as raw yuv420p (compare with ffmpeg -f rawvideo),
 * -a the audio as 48 kHz s16 stereo, -p frame N as a PPM through yuv_to_rgb.
 * Built by tools/codecs/build.sh with musl, statically, like Reel itself. */
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv){
    if(argc < 2){ fprintf(stderr, "usage: mediatest FILE [-y out.yuv] [-a out.s16] [-p N out.ppm] [-c chunk] [-s W H]\n"); return 2; }
    const char *yuv_out = NULL, *pcm_out = NULL, *ppm_out = NULL;
    int ppm_n = -1, chunk = 0, sw = 0, sh = 0;
    for(int i = 2; i < argc; i++){
        if(!strcmp(argv[i], "-y") && i + 1 < argc) yuv_out = argv[++i];
        else if(!strcmp(argv[i], "-a") && i + 1 < argc) pcm_out = argv[++i];
        else if(!strcmp(argv[i], "-p") && i + 2 < argc){ ppm_n = atoi(argv[++i]); ppm_out = argv[++i]; }
        else if(!strcmp(argv[i], "-c") && i + 1 < argc) chunk = atoi(argv[++i]);
        else if(!strcmp(argv[i], "-s") && i + 2 < argc){ sw = atoi(argv[++i]); sh = atoi(argv[++i]); }
    }
    FILE *f = fopen(argv[1], "rb");
    if(!f){ perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if(fread(data, 1, (size_t)n, f) != (size_t)n){ perror("read"); return 1; }
    fclose(f);

    static sbuf_t sb;
    sbuf_init(&sb);
    double t0 = now();
    long at = 0;
    unsigned seed = 12345;
    while(at < n){
        long take = chunk ? chunk : (long)((seed = seed * 1103515245 + 12345) >> 16) % 70000 + 1;
        if(take > n - at) take = n - at;
        if(sbuf_append(&sb, data + at, (uint32_t)take) != 0){ fprintf(stderr, "append: %s (at %ld)\n", sb.errmsg, at); break; }
        at += take;
    }
    double t1 = now();
    printf("parsed %ld bytes in %.1f ms, %u bytes left unparsed, duration %.2f s\n", n, (t1 - t0) * 1000, sb.in_len,
           sb.duration_us / 1e6);
    for(int i = 0; i < sb.ntracks; i++){
        mtrack_t *t = &sb.tracks[i];
        int keys = 0;
        for(int k = 0; k < t->n; k++) keys += t->s[k].key;
        printf("track %d: %s %s %dx%d %d Hz %d ch, extra %d, %d frames (%d key)\n", t->id,
               t->kind == MK_VIDEO ? "video" : t->kind == MK_AUDIO ? "audio" : "other",
               media_codec_name(t->codec), t->width, t->height, t->rate, t->channels, t->extralen, t->n, keys);
        int64_t r[16];
        int nr = sbuf_buffered(t, r, 8);
        for(int k = 0; k < nr; k++) printf("  buffered %.3f - %.3f\n", r[k * 2] / 1e6, r[k * 2 + 1] / 1e6);
        if(t->n) printf("  first pts %.3f dts %.3f\n", t->s[0].pts / 1e6, t->s[0].dts / 1e6);
    }

    for(int i = 0; i < sb.ntracks; i++){
        mtrack_t *t = &sb.tracks[i];
        if(!t->codec || !t->n) continue;
        char err[128];
        mdec_t *d = mdec_open(t, err, sizeof err);
        if(!d){ printf("open: %s\n", err); continue; }
        FILE *out = NULL;
        if(t->kind == MK_VIDEO && yuv_out) out = fopen(yuv_out, "wb");
        if(t->kind == MK_AUDIO && pcm_out) out = fopen(pcm_out, "wb");
        int frames = 0, errors = 0;
        long audio_frames = 0;
        int64_t first_pts = -1, last_pts = -1;
        double conv = 0;
        uint32_t *rgb = NULL;
        static int16_t pcm[8192 * 2];
        double d0 = now();
        for(int k = 0; k <= t->n; k++){
            if(k < t->n){
                int r;
                while((r = mdec_send(d, &t->s[k])) == 1){
                    /* full: take output first */
                    mpic_t p; int64_t ap;
                    if(t->kind == MK_VIDEO){ if(mdec_picture(d, &p) != 1) break; frames++; }
                    else if(mdec_audio(d, pcm, 8192, &ap) <= 0) break;
                }
                if(r < 0) errors++;
            } else mdec_drain(d);
            for(;;){
                if(t->kind == MK_VIDEO){
                    mpic_t p;
                    int r = mdec_picture(d, &p);
                    if(r < 0){ errors++; break; }
                    if(r == 0) break;
                    if(first_pts < 0) first_pts = p.pts;
                    last_pts = p.pts;
                    if(out){
                        for(int y = 0; y < p.h; y++) fwrite(p.y + (size_t)y * p.ys, 1, (size_t)p.w, out);
                        for(int y = 0; y < p.ch; y++) fwrite(p.u + (size_t)y * p.uvs, 1, (size_t)p.cw, out);
                        for(int y = 0; y < p.ch; y++) fwrite(p.v + (size_t)y * p.uvs, 1, (size_t)p.cw, out);
                    }
                    int ow = sw ? sw : p.w, oh = sh ? sh : p.h;
                    if(!rgb) rgb = malloc((size_t)ow * oh * 4);
                    double c0 = now();
                    yuv_to_rgb(&p, rgb, ow, oh, ow);
                    conv += now() - c0;
                    if(frames == ppm_n && ppm_out){
                        FILE *pp = fopen(ppm_out, "wb");
                        fprintf(pp, "P6\n%d %d\n255\n", ow, oh);
                        for(int q = 0; q < ow * oh; q++){ uint8_t c[3] = { rgb[q] >> 16, rgb[q] >> 8, rgb[q] }; fwrite(c, 1, 3, pp); }
                        fclose(pp);
                    }
                    frames++;
                } else {
                    int64_t ap;
                    int got = mdec_audio(d, pcm, 8192, &ap);
                    if(got < 0){ errors++; break; }
                    if(got == 0) break;
                    if(first_pts < 0) first_pts = ap;
                    last_pts = ap;
                    audio_frames += got;
                    if(out) fwrite(pcm, 4, (size_t)got, out);
                }
            }
        }
        double d1 = now();
        if(t->kind == MK_VIDEO)
            printf("decoded %d pictures (%d errors) in %.0f ms: %.2f ms each, yuv->rgb %.2f ms each; pts %.3f..%.3f\n",
                   frames, errors, (d1 - d0) * 1000, frames ? (d1 - d0 - conv) * 1000 / frames : 0,
                   frames ? conv * 1000 / frames : 0, first_pts / 1e6, last_pts / 1e6);
        else
            printf("decoded %.2f s of audio (%d errors) in %.0f ms; pts %.3f..%.3f\n",
                   audio_frames / 48000.0, errors, (d1 - d0) * 1000, first_pts / 1e6, last_pts / 1e6);
        if(out) fclose(out);
        free(rgb);
        mdec_close(d);
    }
    sbuf_free(&sb);
    free(data);
    return 0;
}
