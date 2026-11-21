/* POST /v1/audio/speech: OpenAI-compatible text-to-speech from a ThimbleTTS voice file. */
#include "server_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void srv_load_voice(srv *s) {
    const char *want = s->cfg->voice;
    if (!strcmp(want, "off")) return;
    if (!thimble_voice_find(want, s->voice_path, sizeof s->voice_path)) {
        if (s->cfg->log_level >= 1) {
            char dir[1024]; thimble_voice_dir(dir, sizeof dir);
            if (want[0]) fprintf(stderr, "voice '%s' not found; speech is off\n", want);
            else fprintf(stderr, "no voice installed in %s; speech is off (install one: python3 -m thimble voice install)\n", dir);
        }
        return;
    }
    if (s->cfg->voice_threads > 0) thimble_voice_threads(s->cfg->voice_threads);
    char err[256];
    s->voice = thimble_voice_load(s->voice_path, err, sizeof err);
    if (!s->voice) { fprintf(stderr, "error: cannot load voice %s: %s\n", s->voice_path, err); return; }
    if (s->cfg->log_level >= 1)
        fprintf(stderr, "loaded voice '%s' from %s (%d thread%s)\n", thimble_voice_name(s->voice), s->voice_path,
                thimble_voice_threads(0), thimble_voice_threads(0) == 1 ? "" : "s");
}

static void le32(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8 & 255; p[2] = v >> 16 & 255; p[3] = v >> 24; }

void srv_handle_speech(srv *s, http_request *req, const json *body) {
    const char *input = json_str(json_get(body, "input"), NULL);
    const char *fmt = json_str(json_get(body, "response_format"), "wav");
    if (!input || !*input) { http_send_error(req, 422, "'input' is required and must be a non-empty string"); return; }
    if (strlen(input) > 4000) { http_send_error(req, 422, "'input' is longer than 4000 characters"); return; }
    if (strcmp(fmt, "wav") && strcmp(fmt, "pcm")) { http_send_error(req, 400, "response_format must be 'wav' or 'pcm'"); return; }
    if (!s->voice) {
        char dir[1024]; thimble_voice_dir(dir, sizeof dir);
        char msg[1400];
        snprintf(msg, sizeof msg, "no voice installed; put a .voice file in %s (python3 -m thimble voice install)", dir);
        http_send_error(req, 503, msg);
        return;
    }
    int16_t *pcm = NULL; char err[256] = "";
    pthread_mutex_lock(&s->voice_mu);
    int n = thimble_voice_speak(s->voice, input, &pcm, err, sizeof err);
    pthread_mutex_unlock(&s->voice_mu);
    if (n < 0) { http_send_error(req, n == -1 ? 500 : 400, err[0] ? err : "could not speak that text"); return; }
    const size_t bytes = (size_t) n * 2;
    if (!strcmp(fmt, "pcm")) { http_send(req, 200, "audio/L16;rate=24000", (const char *) pcm, bytes); free(pcm); return; }
    unsigned char *wav = malloc(44 + bytes);
    if (!wav) { free(pcm); http_send_error(req, 500, "out of memory"); return; }
    const unsigned rate = (unsigned) thimble_voice_sample_rate(s->voice);
    memcpy(wav, "RIFF", 4); le32(wav + 4, (unsigned) (36 + bytes)); memcpy(wav + 8, "WAVEfmt ", 8);
    le32(wav + 16, 16); wav[20] = 1; wav[21] = 0; wav[22] = 1; wav[23] = 0;
    le32(wav + 24, rate); le32(wav + 28, rate * 2); wav[32] = 2; wav[33] = 0; wav[34] = 16; wav[35] = 0;
    memcpy(wav + 36, "data", 4); le32(wav + 40, (unsigned) bytes);
    memcpy(wav + 44, pcm, bytes);
    free(pcm);
    http_send(req, 200, "audio/wav", (const char *) wav, 44 + bytes);
    free(wav);
}
