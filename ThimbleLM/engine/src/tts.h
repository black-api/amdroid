/* ThimbleTTS in C: internal types shared by the tts_*.c files. Public API: include/thimble.h */
#ifndef THIMBLE_TTS_H
#define THIMBLE_TTS_H

#include <stddef.h>
#include <stdint.h>

/* ---- the single-byte symbol alphabet (must equal SYMBOLS in thimble/tts/pack.py) ---- */
#define TTS_SYMBOLS_UTF8 "æðŋɑɔəɛɜɡɪɹɾʃʊʌʒʤʧˈˌθᵊᵻʔɐ—“”…‘’«»¿¡£€–"
#define TTS_N_SYMBOLS 38
#define TTS_OTHER 0xA6                 /* any other non-ASCII character */
enum {
    S_AE = 0x80, S_ETH, S_ENG, S_OPENA, S_OPENO, S_SCHWA, S_OPENE, S_REVE, S_G, S_IH, S_R,
    S_TAP, S_SH, S_UH, S_WEDGE, S_ZH, S_DJ, S_TCH, S_PRIMARY, S_SECONDARY, S_THETA,
    S_SCHWAS, S_IBAR, S_GLOTTAL, S_AEXT, S_EMDASH, S_LDQUOTE, S_RDQUOTE, S_ELLIPSIS,
    S_LSQUOTE, S_RSQUOTE, S_LGUIL, S_RGUIL, S_IQUEST, S_IEXCL, S_POUND, S_EURO, S_ENDASH
};

/* ---- a loaded safetensors file ---- */
typedef struct { char *name; int dtype; int ndim; int shape[4]; size_t off, end; } tts_tensor;
typedef struct { char *key, *val; } tts_meta;
enum { TT_F32 = 0, TT_I32 = 1, TT_U8 = 2 };

typedef struct {
    unsigned char *buf; size_t size, data0;
    tts_tensor *t; int nt;
    tts_meta *m; int nm;
} tts_file;

int         tts_file_open(tts_file *f, const char *path, char *err, size_t err_len);
void        tts_file_close(tts_file *f);
const tts_tensor *tts_file_find(const tts_file *f, const char *name);
const void *tts_file_data(const tts_file *f, const tts_tensor *t);
const char *tts_file_meta(const tts_file *f, const char *key);

/* ---- the three networks ---- */
typedef struct { const float *w0, *b0, *w1, *b1; float scale; } tts_res;
typedef struct {
    int H, depth, vocab;
    const float *embed, *proj_w, *proj_b, *out_w, *out_b;
    tts_res *blk;
} tts_dur;
typedef struct {
    int H, token_depth, depth, n_mels, vocab;
    const float *embed, *tproj_w, *tproj_b, *fproj_w, *fproj_b, *out_w, *out_b;
    tts_res *tblk, *fblk;
} tts_ac;
typedef struct {
    const float *dw_w, *dw_b, *norm_w, *norm_b, *pw0_w, *pw0_b, *pw1_w, *pw1_b, *gamma;
} tts_cnx;
typedef struct {
    int dim, pw, nblk, n_mels, bins;
    const float *embed_w, *embed_b, *noise_b, *norm_w, *norm_b, *fnorm_w, *fnorm_b, *head_w, *head_b;
    tts_cnx *blk;
} tts_dec;

/* ---- the grapheme-to-phoneme fallback (BART, 1 head) ---- */
typedef struct {
    const float *q_w, *q_b, *k_w, *k_b, *v_w, *v_b, *o_w, *o_b;
} tts_attn;
typedef struct {
    tts_attn self, cross;
    const float *self_ln_w, *self_ln_b, *cross_ln_w, *cross_ln_b, *fc1_w, *fc1_b, *fc2_w, *fc2_b,
                *final_ln_w, *final_ln_b;
} tts_layer;
typedef struct {
    int d, ff, n_tokens, max_pos, bos, eos, start, offset;
    float eps;
    const float *embed, *logit_bias, *enc_pos, *dec_pos, *enc_ln_w, *enc_ln_b, *dec_ln_w, *dec_ln_b;
    tts_layer enc, dec;
    int g2id[256];           /* grapheme byte -> id (or -1) */
    unsigned char ph_chars[128];  /* id -> phoneme symbol code */
    int n_ph;
} tts_oov;

/* ---- sorted pronunciation dictionary ---- */
typedef struct { const unsigned char *blob; const int32_t *index; int n; } tts_dict;

struct thimble_voice {
    tts_file file;
    char name[96];
    int sample_rate;
    tts_dur dur;
    tts_ac ac;
    tts_dec dec;
    const float *tilt;
    tts_oov oov;
    tts_dict gold, silver;
};
typedef struct thimble_voice thimble_voice;

/* tts_par.c: run fn over [0, n) split across threads; iterations must be independent. */
typedef void (*tts_par_fn)(void *ctx, int begin, int end);
void tts_parallel_for(int n, tts_par_fn fn, void *ctx);
int  tts_threads(void);           /* $THIMBLE_VOICE_THREADS, else cores (max 8) */
void tts_set_threads(int n);

/* tts_net.c */
int tts_net_init(thimble_voice *v, char *err, size_t n);
void tts_net_free(thimble_voice *v);
int tts_durations(const thimble_voice *v, const int *ids, int n, int *durs);
/* ids -> waveform (float, un-normalised). Caller frees *wav. Returns samples or -1. */
int tts_synth_ids(const thimble_voice *v, const int *ids, int n, float **wav);

/* tts_oov.c */
int tts_oov_init(thimble_voice *v, char *err, size_t n);
int tts_oov_word(const thimble_voice *v, const unsigned char *word, int len, unsigned char *out, int cap);

/* tts_g2p.c: text -> ids. Returns the id count, or <0: -2 empty, -3 too long, -1 other (err set). */
int tts_dict_init(thimble_voice *v, char *err, size_t n);
int tts_phonemize(const thimble_voice *v, const char *text, int *ids, int max_ids, char *err, size_t err_len);
/* text -> phoneme string as UTF-8 IPA (debug / tests); returns length or -1 */
int tts_phoneme_string(const thimble_voice *v, const char *text, char *out, size_t cap);
/* one UTF-8 character -> its single-byte code (ASCII as itself); *adv = bytes consumed */
int tts_decode_utf8(const unsigned char *s, size_t n, size_t *adv);
size_t tts_utf8_symbols(const unsigned char *codes, size_t n, char *out, size_t cap);

#endif
