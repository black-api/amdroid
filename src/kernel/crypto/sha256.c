/* sha256.c — SHA-256 (FIPS 180-4) for password hashing. */
#include "amdroid/sha256.h"
#include <string.h>

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(u32 h[8], const u8 *p){
    u32 w[64];
    for(int i = 0; i < 16; i++)
        w[i] = ((u32)p[4*i] << 24) | ((u32)p[4*i+1] << 16) | ((u32)p[4*i+2] << 8) | p[4*i+3];
    for(int i = 16; i < 64; i++){
        u32 s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        u32 s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u32 a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
    for(int i = 0; i < 64; i++){
        u32 S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K[i] + w[i];
        u32 S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256(const void *data, u32 len, u8 out[32]){
    u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const u8 *p = data;
    u32 full = len / 64;
    for(u32 i = 0; i < full; i++) block(h, p + 64*i);
    /* final block(s): remaining bytes, 0x80, zero padding, 64-bit bit length */
    u8 tail[128];
    u32 rem = len - full*64;
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + full*64, rem);
    tail[rem] = 0x80;
    u32 blocks = (rem + 1 + 8 <= 64) ? 1 : 2;
    u64 bits = (u64)len * 8;
    u8 *end = tail + blocks*64;
    for(int i = 0; i < 8; i++) end[-1-i] = (u8)(bits >> (8*i));
    for(u32 b = 0; b < blocks; b++) block(h, tail + 64*b);
    for(int i = 0; i < 8; i++){
        out[4*i] = (u8)(h[i] >> 24); out[4*i+1] = (u8)(h[i] >> 16);
        out[4*i+2] = (u8)(h[i] >> 8); out[4*i+3] = (u8)h[i];
    }
}

/* HMAC-SHA256 (RFC 2104). */
void hmac_sha256(const void *key, u32 klen, const void *msg, u32 mlen, u8 out[32]){
    u8 k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, sizeof k);
    if(klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    for(int i = 0; i < 64; i++){ ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    /* inner = H(ipad || msg) */
    u8 *buf = (u8*)0;
    (void)buf;
    /* messages are limited to 256 bytes here (the callers use far less); longer
       input gets an all-zero result rather than a silently shortened one */
    if(mlen > 256){ memset(out, 0, 32); return; }
    u8 tmp[64 + 256];
    memcpy(tmp, ipad, 64);
    memcpy(tmp + 64, msg, mlen);
    sha256(tmp, 64 + mlen, inner);
    u8 o[96];
    memcpy(o, opad, 64);
    memcpy(o + 64, inner, 32);
    sha256(o, 96, out);
}

/* PBKDF2 with HMAC-SHA256 (RFC 8018). */
void pbkdf2_sha256(const void *pass, u32 plen, const void *salt, u32 slen, u32 iterations, u8 *out, u32 outlen){
    u32 block = 1;
    while(outlen){
        u8 sb[80];
        memcpy(sb, salt, slen);
        sb[slen] = (u8)(block >> 24); sb[slen+1] = (u8)(block >> 16);
        sb[slen+2] = (u8)(block >> 8); sb[slen+3] = (u8)block;
        u8 u[32], t[32];
        hmac_sha256(pass, plen, sb, slen + 4, u);
        memcpy(t, u, 32);
        for(u32 i = 1; i < iterations; i++){
            hmac_sha256(pass, plen, u, 32, u);
            for(int k = 0; k < 32; k++) t[k] ^= u[k];
        }
        u32 n = outlen < 32 ? outlen : 32;
        memcpy(out, t, n);
        out += n; outlen -= n;
        block++;
    }
}
