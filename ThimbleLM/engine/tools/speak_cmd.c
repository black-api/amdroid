/* thimble speak "text" [--voice NAME|PATH] [--out FILE.wav]   and   thimble voice [info]
 *
 * Text to speech with a ThimbleTTS voice file; no Python, no model file needed.
 */
#include "thimble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void le32(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8 & 255; p[2] = v >> 16 & 255; p[3] = v >> 24; }

static int write_wav(const char *path, const int16_t *pcm, int n, unsigned rate) {
    FILE *f = strcmp(path, "-") ? fopen(path, "wb") : stdout;
    if (!f) { fprintf(stderr, "error: cannot write %s\n", path); return -1; }
    unsigned char h[44]; const unsigned bytes = (unsigned) n * 2;
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0; le32(h + 24, rate); le32(h + 28, rate * 2);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4); le32(h + 40, bytes);
    fwrite(h, 1, 44, f); fwrite(pcm, 2, (size_t) n, f);
    if (f != stdout) fclose(f);
    return 0;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double) t.tv_sec + (double) t.tv_nsec * 1e-9; }

static int find_voice(const char *name, char *path, size_t len) {
    if (thimble_voice_find(name, path, len)) return 0;
    char dir[1024]; thimble_voice_dir(dir, sizeof dir);
    if (name && *name) fprintf(stderr, "error: voice '%s' not found (looked in %s)\n", name, dir);
    else fprintf(stderr, "error: no voice installed in %s\n", dir);
    fprintf(stderr, "install one with:  python3 -m thimble voice install FILE_OR_URL\n");
    return -1;
}

/* Read all of stdin, so an answer can be piped straight into the voice. */
static char *read_stdin(void) {
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; char *g = realloc(buf, cap); if (!g) { free(buf); return NULL; } buf = g; }
        size_t got = fread(buf + n, 1, 4096, stdin);
        n += got;
        if (got < 4096) break;
    }
    buf[n] = 0;
    return buf;
}

int speak_main(int argc, char **argv) {
    const char *voice = "", *out = "speech.wav", *text = NULL;
    int threads = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--voice") && i + 1 < argc) voice = argv[++i];
        else if ((!strcmp(argv[i], "--out") || !strcmp(argv[i], "-o")) && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "-")) { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else text = argv[i];
    }
    char *from_stdin = NULL;
    /* "speak -" always reads stdin; with no text at all, read it only when stdin
     * is a pipe or a file, so an interactive "thimble speak" prints usage
     * instead of silently waiting for input. */
    if ((text && !strcmp(text, "-")) || (!text && !isatty(0))) {
        from_stdin = read_stdin();
        text = from_stdin;
        if (!text || !*text) {
            free(from_stdin);
            fprintf(stderr, "usage: thimble speak \"text\" [--voice NAME|PATH] [--out FILE.wav|-] [--threads N]\n"
                            "       ... | thimble speak -          (read the text from stdin)\n");
            return 2;
        }
    }
    if (threads > 0) thimble_voice_threads(threads);
    char path[1024], err[256];
    if (find_voice(voice, path, sizeof path)) { free(from_stdin); return 1; }
    double t0 = now();
    thimble_voice *v = thimble_voice_load(path, err, sizeof err);
    if (!v) { fprintf(stderr, "error: %s\n", err); free(from_stdin); return 1; }
    double t1 = now();
    int16_t *pcm = NULL;
    int n = thimble_voice_speak(v, text, &pcm, err, sizeof err);
    double t2 = now();
    if (n < 0) { fprintf(stderr, "error: %s\n", err[0] ? err : "could not speak that text"); thimble_voice_free(v); free(from_stdin); return n == -1 ? 1 : 2; }
    int rc = write_wav(out, pcm, n, (unsigned) thimble_voice_sample_rate(v)) ? 1 : 0;
    const double secs = (double) n / thimble_voice_sample_rate(v);
    fprintf(stderr, "%s: %.2f s of audio, loaded in %.0f ms, spoken in %.0f ms (%.0fx real time, %d thread%s)\n",
            rc ? "failed" : out, secs, (t1 - t0) * 1e3, (t2 - t1) * 1e3, secs / (t2 - t1),
            thimble_voice_threads(0), thimble_voice_threads(0) == 1 ? "" : "s");
    free(pcm);
    free(from_stdin);
    thimble_voice_free(v);
    return rc;
}

int voice_main(int argc, char **argv) {
    char dir[1024]; thimble_voice_dir(dir, sizeof dir);
    const char *name = argc > 0 ? argv[0] : "";
    char path[1024], err[256];
    if (!thimble_voice_find(name, path, sizeof path)) {
        printf("no voice installed (voice folder: %s)\ninstall one with:  python3 -m thimble voice install FILE_OR_URL\n", dir);
        return 1;
    }
    thimble_voice *v = thimble_voice_load(path, err, sizeof err);
    if (!v) { fprintf(stderr, "error: %s\n", err); return 1; }
    printf("voice: %s\nfile: %s\nsample rate: %d Hz\n", thimble_voice_name(v), path, thimble_voice_sample_rate(v));
    thimble_voice_free(v);
    return 0;
}
