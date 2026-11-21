/* Voice loading and the public API (text -> speech). */
#include "tts.h"
#include "../include/thimble.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#include <sys/stat.h>

static void seterr(char *err, size_t n, const char *msg) { if (err && n) snprintf(err, n, "%s", msg); }

thimble_voice *thimble_voice_load(const char *path, char *err, size_t err_len) {
    thimble_voice *v = calloc(1, sizeof *v);
    if (!v) { seterr(err, err_len, "out of memory"); return NULL; }
    if (tts_file_open(&v->file, path, err, err_len)) { free(v); return NULL; }
    const char *fmt = tts_file_meta(&v->file, "format"), *sym = tts_file_meta(&v->file, "symbols");
    if (!fmt || strcmp(fmt, "thimble-voice")) { seterr(err, err_len, "not a thimble voice file"); goto fail; }
    if (!sym || strcmp(sym, TTS_SYMBOLS_UTF8)) { seterr(err, err_len, "voice file uses a different symbol table (rebuild it with this version)"); goto fail; }
    const char *name = tts_file_meta(&v->file, "name"), *sr = tts_file_meta(&v->file, "sample_rate");
    snprintf(v->name, sizeof v->name, "%s", name ? name : "voice");
    v->sample_rate = sr ? atoi(sr) : 24000;
    if (tts_net_init(v, err, err_len) || tts_oov_init(v, err, err_len) || tts_dict_init(v, err, err_len)) goto fail;
    return v;
fail:
    thimble_voice_free(v);
    return NULL;
}

void thimble_voice_free(thimble_voice *v) {
    if (!v) return;
    tts_net_free(v);
    tts_file_close(&v->file);
    free(v);
}

int thimble_voice_sample_rate(const thimble_voice *v) { return v->sample_rate; }

int thimble_voice_threads(int n) {
    if (n > 0) tts_set_threads(n);
    return tts_threads();
}
const char *thimble_voice_name(const thimble_voice *v) { return v->name; }
const char *thimble_voice_license(const thimble_voice *v) { return tts_file_meta(&v->file, "license"); }

int thimble_voice_ids(const thimble_voice *v, const char *text, int *ids, int max_ids, char *err, size_t err_len) {
    return tts_phonemize(v, text, ids, max_ids, err, err_len);
}

int thimble_voice_phonemes(const thimble_voice *v, const char *text, char *out, size_t cap) {
    return tts_phoneme_string(v, text, out, cap);
}

int thimble_voice_synth_ids(const thimble_voice *v, const int *ids, int n, float **wav) {
    return tts_synth_ids(v, ids, n, wav);
}

/* ---- text -> chunks -> audio, as ThimbleTTS._chunks / synthesize ---- */
#define SOFT_FRAMES 2000
#define MAX_IDS 207
#define MAX_TOK (MAX_IDS - 7)

typedef struct { char *s; } piece;

static int size_of(const thimble_voice *v, const char *s, int *frames, int *tokens, char *err, size_t en) {
    int ids[MAX_IDS * 4];
    int n = tts_phonemize(v, s, ids, MAX_IDS * 4, err, en);
    if (n == -3) { *frames = SOFT_FRAMES + 1; *tokens = MAX_TOK + 1; return 0; }
    if (n == -2) { *frames = 0; *tokens = 0; return 0; }
    if (n < 0) return n;
    int durs[MAX_IDS * 4];
    if (tts_durations(v, ids, n, durs)) return -1;
    int f = 0; for (int i = 0; i < n; i++) f += durs[i];
    *frames = f; *tokens = n;
    return 0;
}

/* split `text` at sentence ends (after . ! ? … :), like re.split(r"(?<=[.!?…:])\s+") */
static int split_sentences(const char *text, char ***out) {
    size_t n = strlen(text);
    char **v = malloc(sizeof(char *) * (n / 1 + 2)); int nv = 0;
    size_t start = 0, i = 0;
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) i++;
    start = i;
    while (i < n) {
        int is_end = text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == ':';
        if (!is_end && (unsigned char)text[i] == 0xE2 && (unsigned char)text[i + 1] == 0x80 && (unsigned char)text[i + 2] == 0xA6) { is_end = 1; i += 2; }
        if (is_end && i + 1 < n && (text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '\n' || text[i + 1] == '\r')) {
            size_t e = i + 1, j = e;
            while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r')) j++;
            if (j < n) {
                v[nv] = malloc(e - start + 1); memcpy(v[nv], text + start, e - start); v[nv][e - start] = 0; nv++;
                start = j; i = j; continue;
            }
        }
        i++;
    }
    size_t e = n; while (e > start && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r')) e--;
    if (e > start) { v[nv] = malloc(e - start + 1); memcpy(v[nv], text + start, e - start); v[nv][e - start] = 0; nv++; }
    *out = v;
    return nv;
}

int thimble_voice_speak(const thimble_voice *v, const char *text, int16_t **pcm_out, char *err, size_t err_len) {
    *pcm_out = NULL;
    if (err && err_len) err[0] = 0;
    char **parts; int np = split_sentences(text, &parts);
    if (np == 0) { seterr(err, err_len, "text must be a non-empty string"); free(parts); return -2; }

    /* a piece that does not fit one pass is cut at word boundaries */
    char **chunks = malloc(sizeof(char *) * (strlen(text) + 2)); int nc = 0;
    int rc = 0;
    for (int p = 0; p < np && !rc; p++) {
        int fr, tk;
        if ((rc = size_of(v, parts[p], &fr, &tk, err, err_len))) break;
        if (fr <= SOFT_FRAMES && tk <= MAX_TOK) { chunks[nc++] = parts[p]; parts[p] = NULL; continue; }
        char *cur = calloc(strlen(parts[p]) + 2, 1); int cfr = 0, ctk = 0;
        for (char *w = strtok(parts[p], " \t\n\r"); w; w = strtok(NULL, " \t\n\r")) {
            int fw, nw;
            if ((rc = size_of(v, w, &fw, &nw, err, err_len))) break;
            nw = nw > 0 ? nw - 1 : 0;
            if (cur[0] && (cfr + fw > SOFT_FRAMES || ctk + nw > MAX_TOK)) {
                chunks[nc++] = cur; cur = calloc(strlen(parts[p]) + strlen(text) + 2, 1); cfr = fw; ctk = nw; strcpy(cur, w);
            } else {
                if (cur[0]) strcat(cur, " ");
                strcat(cur, w); cfr += fw; ctk += nw;
            }
        }
        if (cur[0]) chunks[nc++] = cur; else free(cur);
    }
    for (int p = 0; p < np; p++) free(parts[p]);
    free(parts);

    float *all = NULL; size_t nall = 0;
    const size_t gap = (size_t)(0.10 * v->sample_rate);
    for (int c = 0; c < nc && !rc; c++) {
        int ids[MAX_IDS * 4];
        int n = tts_phonemize(v, chunks[c], ids, MAX_IDS * 4, err, err_len);
        if (n < 0) { rc = n; break; }
        float *w; int ns = tts_synth_ids(v, ids, n, &w);
        if (ns < 0) { rc = -1; seterr(err, err_len, "synthesis failed"); break; }
        all = realloc(all, sizeof(float) * (nall + (size_t)ns + gap));
        memcpy(all + nall, w, sizeof(float) * (size_t)ns); nall += (size_t)ns; free(w);
        if (c < nc - 1) { memset(all + nall, 0, sizeof(float) * gap); nall += gap; }
    }
    for (int c = 0; c < nc; c++) free(chunks[c]);
    free(chunks);
    if (rc) { free(all); return rc < 0 ? rc : -1; }
    if (!nall) { free(all); seterr(err, err_len, "nothing to say"); return -2; }

    /* RMS-normalise to 0.05 and convert to 16-bit, as ThimbleTTS.to_pcm16 */
    double ss = 0; for (size_t i = 0; i < nall; i++) ss += (double)all[i] * all[i];
    const double scale = 0.05 / (sqrt(ss / (double)nall) + 1e-9);
    int16_t *pcm = malloc(sizeof(int16_t) * nall);
    for (size_t i = 0; i < nall; i++) {
        double x = (double)(float)(all[i] * scale);
        if (x > 1.0) x = 1.0; else if (x < -1.0) x = -1.0;
        pcm[i] = (int16_t)(x * 32767.0);
    }
    free(all);
    *pcm_out = pcm;
    return (int)nall;
}

/* ---- locating voices ---- */
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

size_t thimble_voice_dir(char *buf, size_t len) {
    const char *d = getenv("THIMBLE_VOICE_DIR");
    if (d && *d) return (size_t)snprintf(buf, len, "%s", d);
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) return (size_t)snprintf(buf, len, "%s/thimble/voices", x);
    if (h && *h) return (size_t)snprintf(buf, len, "%s/.cache/thimble/voices", h);
    if (len) buf[0] = 0;
    return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int thimble_voice_find(const char *name, char *path, size_t len) {
    if (!name || !*name) {
        const char *f = getenv("THIMBLE_VOICE");
        if (f && *f && is_file(f)) { snprintf(path, len, "%s", f); return 1; }
    } else if (strchr(name, '/') || strstr(name, ".voice")) {
        if (is_file(name)) { snprintf(path, len, "%s", name); return 1; }
    }
    char dir[1024];
    if (!thimble_voice_dir(dir, sizeof dir)) return 0;
    if (name && *name && !strchr(name, '/')) {
        snprintf(path, len, "%s/%s%s", dir, name, strstr(name, ".voice") ? "" : ".voice");
        return is_file(path);
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *names[256]; int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < 256;) {
        size_t l = strlen(e->d_name);
        if (l > 6 && !strcmp(e->d_name + l - 6, ".voice")) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n) { qsort(names, (size_t)n, sizeof *names, cmp_str); snprintf(path, len, "%s/%s", dir, names[0]); }
    for (int i = 0; i < n; i++) free(names[i]);
    return n > 0;
}
