/* Byte-level BPE tokenizer (GPT-2 style), loaded from the GGUF vocab/merges.
 * Mirrors the HF `tokenizers` setup ThimbleLM trains: ByteLevel pre-tokenizer
 * with the GPT-2 split regex, BPE with an unk token, special tokens matched
 * verbatim before pre-tokenization. */
#ifndef THIMBLE_TOKENIZER_H
#define THIMBLE_TOKENIZER_H

#include "gguf.h"
#include "util.h"

/* GGUF token types */
enum { TOK_NORMAL = 1, TOK_UNKNOWN = 2, TOK_CONTROL = 3, TOK_USER = 4, TOK_UNUSED = 5, TOK_BYTE = 6 };

typedef struct {
    int      n_vocab;
    char   **text;       /* token strings (byte-level alphabet), NUL-terminated */
    int     *text_len;
    int     *type;
    strmap   vocab;      /* token string -> id */
    pairmap  merges;     /* (left id, right id) -> (rank, merged id) */
    int     *special;    /* CONTROL ids, longest string first */
    int      n_special;
    char     byte_enc[256][3];   /* byte -> UTF-8 of its byte-level code point */
    int      byte_enc_len[256];
    int      byte_dec[512];      /* code point -> byte (-1 if none) */
    int      bos, eos, pad, unk;
    int      think, think_end;   /* -1 when the vocab has no think tokens */
} thimble_tokenizer;

int  tokenizer_load(thimble_tokenizer *t, const gguf_file *f, char *err, size_t err_len);
void tokenizer_free(thimble_tokenizer *t);

/* Returns the total number of ids; writes up to max_out of them. */
int tokenizer_encode(const thimble_tokenizer *t, const char *text, int *out, int max_out);

/* Bytes for one id (0 for CONTROL/UNUSED unless with_special). */
int tokenizer_piece(const thimble_tokenizer *t, int id, char *buf, int len, int with_special);

int tokenizer_find(const thimble_tokenizer *t, const char *token);   /* id or -1 */

#endif
