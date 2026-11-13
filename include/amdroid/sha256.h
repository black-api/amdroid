#ifndef AMDROID_SHA256_H
#define AMDROID_SHA256_H
#include "amdroid/types.h"
void sha256(const void *data, u32 len, u8 out[32]);
void hmac_sha256(const void *key, u32 klen, const void *msg, u32 mlen, u8 out[32]);
void pbkdf2_sha256(const void *pass, u32 plen, const void *salt, u32 slen, u32 iterations, u8 *out, u32 outlen);
#endif
