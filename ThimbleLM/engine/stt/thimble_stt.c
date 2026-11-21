/* thimble-stt: transcribe a 16-bit WAV with a .thim speech model.
 *   thimble-stt MODEL.thim FILE.wav [--full] [--bench N] [--max-tokens N]
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/thimble_stt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static unsigned char *slurp(const char *p, size_t *n){
    FILE *f = fopen(p, "rb"); if(!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = aligned_alloc(64, ((size_t)l + 63) / 64 * 64);
    if(!b || fread(b, 1, (size_t)l, f) != (size_t)l){ fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}

/* 16-bit PCM WAV, mono or stereo (averaged), any rate (linear resample to the model's) */
static float *read_wav(const char *p, int rate, int *n_out){
    size_t n; unsigned char *b = slurp(p, &n);
    if(!b || n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)){ fprintf(stderr, "not a WAV file\n"); return NULL; }
    int ch = 1, sr = 16000, bits = 16; size_t off = 12, dlen = 0, doff = 0;
    while(off + 8 <= n){
        unsigned csz = b[off + 4] | b[off + 5] << 8 | b[off + 6] << 16 | (unsigned)b[off + 7] << 24;
        if(!memcmp(b + off, "fmt ", 4)){ ch = b[off + 10] | b[off + 11] << 8; sr = b[off + 12] | b[off + 13] << 8 | b[off + 14] << 16 | b[off + 15] << 24; bits = b[off + 22] | b[off + 23] << 8; }
        if(!memcmp(b + off, "data", 4)){ doff = off + 8; dlen = csz; if(doff + dlen > n) dlen = n - doff; break; }
        off += 8 + csz + (csz & 1);
    }
    if(bits != 16 || !doff){ fprintf(stderr, "need 16-bit PCM\n"); return NULL; }
    int frames = (int)(dlen / (2 * ch));
    float *src = malloc((size_t)frames * sizeof(float));
    for(int i = 0; i < frames; i++){ float a = 0; for(int c = 0; c < ch; c++){ short v = (short)(b[doff + (i * ch + c) * 2] | b[doff + (i * ch + c) * 2 + 1] << 8); a += v / 32768.f; } src[i] = a / ch; }
    if(sr == rate){ *n_out = frames; return src; }
    int m = (int)((long long)frames * rate / sr);
    float *dst = malloc((size_t)m * sizeof(float));
    for(int i = 0; i < m; i++){ double x = (double)i * sr / rate; int i0 = (int)x; double f = x - i0; int i1 = i0 + 1 < frames ? i0 + 1 : i0; dst[i] = (float)(src[i0] * (1 - f) + src[i1] * f); }
    free(src); *n_out = m; return dst;
}

extern double thim_stt_prof[8];
int main(int argc, char **argv){
    if(argc < 3){ fprintf(stderr, "usage: %s MODEL.thim FILE.wav [--full] [--bench N] [--max-tokens N]\n", argv[0]); return 2; }
    thim_stt_opts o; memset(&o, 0, sizeof o); int bench = 1;
    for(int i = 3; i < argc; i++){
        if(!strcmp(argv[i], "--full")) o.full_ctx = 1;
        else if(!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--max-tokens") && i + 1 < argc) o.max_tokens = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--pad") && i + 1 < argc) o.pad_ctx = atoi(argv[++i]);
        else if(!strcmp(argv[i], "--min") && i + 1 < argc) o.min_ctx = atoi(argv[++i]);
    }
    size_t mn; unsigned char *mb = slurp(argv[1], &mn);
    if(!mb){ fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    char err[200];
    thim_stt_set_clock(now_us);
    double t0 = now_us();
    thim_stt *s = thim_stt_open(mb, mn, err, sizeof err);
    if(!s){ fprintf(stderr, "open: %s\n", err); return 1; }
    fprintf(stderr, "loaded in %.1f ms\n", (now_us() - t0) / 1000);
    int n; float *pcm = read_wav(argv[2], thim_stt_sample_rate(s), &n);
    if(!pcm) return 1;
    char text[4096];
    for(int r = 0; r < bench; r++){
        int len = thim_stt_transcribe(s, pcm, n, &o, text, sizeof text);
        thim_stt_stats st; thim_stt_stats_get(s, &st);
        fprintf(stderr, "audio %.2f s -> %d enc frames, %d tokens | mel %.0f ms, encoder %.0f ms, decoder %.0f ms | total %.0f ms (%.1fx real time)\n",
                n / 16000.0, st.enc_frames, st.tokens, st.mel_us / 1e3, st.encoder_us / 1e3, st.decoder_us / 1e3,
                (st.mel_us + st.encoder_us + st.decoder_us) / 1e3, n / 16000.0 * 1e6 / (st.mel_us + st.encoder_us + st.decoder_us));
        if(len < 0){ fprintf(stderr, "transcribe failed\n"); return 1; }
    }
    if(getenv("THIM_PROF")) fprintf(stderr, "prof ms: matmul %.0f attention %.0f layernorm %.0f gelu(ffn) %.0f (all runs)\n", thim_stt_prof[0] / 1e3, thim_stt_prof[1] / 1e3, thim_stt_prof[3] / 1e3, thim_stt_prof[2] / 1e3);
    printf("%s\n", text);
    thim_stt_close(s);
    return 0;
}
