/* Images: fetched by the browser over HTTP(S), decoded here, drawn by the layout engine.
 * Formats: PNG (zlib/DEFLATE, all colour types, no interlace), GIF (LZW, first frame,
 * interlace handled), BMP (uncompressed 8/24/32-bit). Anything else is reported as failed
 * and the page keeps the <img> alt text instead. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "string.h"

#define IMAGES_MAX   32
#define IMG_MAX_SIDE 2048
#define IMG_MAX_OUT  (16u * 1024 * 1024)

typedef struct {
    char *url;              /* as written in the page (not resolved) */
    u32 *pixels;            /* 0xRRGGBB, w*h, or NULL while not decoded */
    u32 w, h;
    int state;              /* 0 pending, 1 ready, 2 failed */
} image_t;
static image_t images[IMAGES_MAX];
static int nimages = 0;

void web_images_reset(void){
    for(int i = 0; i < nimages; i++){
        if(images[i].url) kfree(images[i].url);
        if(images[i].pixels) kfree(images[i].pixels);
    }
    nimages = 0;
    memset(images, 0, sizeof images);
}

/* Registers an <img> URL. Returns its index, or -1 when the table is full / url already there. */
int img_register(const char *url){
    if(!url) return -1;
    for(int i = 0; i < nimages; i++){
        if(images[i].url && strcmp(images[i].url, url) == 0) return i;
    }
    if(nimages >= IMAGES_MAX) return -1;
    u32 n = (u32)strlen(url);
    if(n >= 1024) return -1;
    char *copy = kmalloc(n + 1);
    if(!copy) return -1;
    memcpy(copy, url, n + 1);
    images[nimages].url = copy;
    images[nimages].state = 0;
    nimages++;
    return nimages - 1;
}

int web_image_count(void){ return nimages; }
const char *web_image(int i){ return (i >= 0 && i < nimages) ? images[i].url : NULL; }
int web_image_state(int i){ return (i >= 0 && i < nimages) ? images[i].state : 2; }
const u32 *web_image_pixels(int i, u32 *w, u32 *h){
    if(i < 0 || i >= nimages || images[i].state != 1) return NULL;
    if(w) *w = images[i].w;
    if(h) *h = images[i].h;
    return images[i].pixels;
}

/* ================= inflate (RFC 1951) ================= */

typedef struct { const u8 *src; u32 n, pos; } br_t;

static u32 br_bits(br_t *b, int cnt){
    u32 v = 0, lim = b->n * 8;          /* pos is in bits, n in bytes */
    for(int i = 0; i < cnt; i++){
        u32 bit = b->pos < lim ? ((b->src[b->pos >> 3] >> (b->pos & 7)) & 1) : 0;
        v |= bit << i;
        b->pos++;
    }
    return v;
}
static u32 br_align_byte(br_t *b){ b->pos = (b->pos + 7) & ~7u; return 0; }

typedef struct { u16 counts[16]; u16 symbols[288]; } huff_t;

static void huff_build(huff_t *h, const u8 *lengths, int n){
    memset(h->counts, 0, sizeof h->counts);
    for(int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    u16 offs[16] = { 0 };
    for(int l = 1; l < 16; l++) offs[l] = (u16)(offs[l - 1] + h->counts[l - 1]);
    for(int i = 0; i < n; i++) if(lengths[i]) h->symbols[offs[lengths[i]]++] = (u16)i;
}

static int huff_decode(br_t *b, const huff_t *h){
    int code = 0, first = 0, index = 0;
    for(int len = 1; len <= 15; len++){
        code |= (int)br_bits(b, 1);
        int count = h->counts[len];
        if(code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const u16 len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  dist_extra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* Inflates src[0..n) into out (cap bytes). Returns the output size, or (u32)-1 on error. */
static u32 inflate_all(const u8 *src, u32 n, u8 *out, u32 out_cap){
    br_t b = { src, n, 0 };
    u32 produced = 0;
    int last = 0;
    while(!last){
        last = (int)br_bits(&b, 1);
        int type = (int)br_bits(&b, 2);
        if(type == 0){                       /* stored */
            br_align_byte(&b);
            if(b.pos + 32 > b.n * 8) return (u32)-1;
            const u8 *p = b.src + (b.pos >> 3);
            u16 len = (u16)(p[0] | (p[1] << 8));
            u16 nlen = (u16)(p[2] | (p[3] << 8));
            if((len ^ nlen) != 0xFFFF) return (u32)-1;
            b.pos += 32;
            if(b.pos + (u32)len * 8 > b.n * 8) return (u32)-1;
            if(produced + len > out_cap) return (u32)-1;
            memcpy(out + produced, b.src + (b.pos >> 3), len);
            b.pos += (u32)len * 8;
            produced += len;
            continue;
        }
        huff_t lit, dist;
        if(type == 1){                       /* fixed */
            u8 lengths[288];
            for(int i = 0; i < 144; i++) lengths[i] = 8;
            for(int i = 144; i < 256; i++) lengths[i] = 9;
            for(int i = 256; i < 280; i++) lengths[i] = 7;
            for(int i = 280; i < 288; i++) lengths[i] = 8;
            huff_build(&lit, lengths, 288);
            u8 dlengths[32];
            for(int i = 0; i < 32; i++) dlengths[i] = 5;
            huff_build(&dist, dlengths, 32);
        } else if(type == 2){                /* dynamic */
            int hlit = (int)br_bits(&b, 5) + 257;
            int hdist = (int)br_bits(&b, 5) + 1;
            int hclen = (int)br_bits(&b, 4) + 4;
            static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            u8 cl_lengths[19] = { 0 };
            for(int i = 0; i < hclen; i++) cl_lengths[order[i]] = (u8)br_bits(&b, 3);
            huff_t cl;
            huff_build(&cl, cl_lengths, 19);
            u8 lengths[288 + 32] = { 0 };
            int got = 0, total = hlit + hdist;
            while(got < total){
                int sym = huff_decode(&b, &cl);
                if(sym < 0) return (u32)-1;
                if(sym < 16) lengths[got++] = (u8)sym;
                else if(sym == 16){
                    int rep = (int)br_bits(&b, 2) + 3;
                    if(got == 0) return (u32)-1;
                    u8 prev = lengths[got - 1];
                    while(rep-- && got < total) lengths[got++] = prev;
                } else if(sym == 17){ int rep = (int)br_bits(&b, 3) + 3; while(rep-- && got < total) lengths[got++] = 0; }
                else { int rep = (int)br_bits(&b, 7) + 11; while(rep-- && got < total) lengths[got++] = 0; }
            }
            huff_build(&lit, lengths, hlit);
            huff_build(&dist, lengths + hlit, hdist);
        } else return (u32)-1;               /* reserved */
        for(;;){
            int sym = huff_decode(&b, &lit);
            if(sym < 0) return (u32)-1;
            if(sym < 256){
                if(produced >= out_cap) return (u32)-1;
                out[produced++] = (u8)sym;
                continue;
            }
            if(sym == 256) break;            /* end of block */
            if(sym > 285) return (u32)-1;
            int len = len_base[sym - 257] + (int)br_bits(&b, len_extra[sym - 257]);
            int dsym = huff_decode(&b, &dist);
            if(dsym < 0 || dsym > 29) return (u32)-1;
            int dlen = dist_base[dsym] + (int)br_bits(&b, dist_extra[dsym]);
            if((u32)dlen > produced) return (u32)-1;
            if(produced + (u32)len > out_cap) return (u32)-1;
            for(int i = 0; i < len; i++){ out[produced] = out[produced - (u32)dlen]; produced++; }
        }
    }
    return produced;
}

/* ================= PNG ================= */

static u32 rd32be(const u8 *p){ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u32 paeth(int a, int b, int c){
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if(pa <= pb && pa <= pc) return (u32)a;
    if(pb <= pc) return (u32)b;
    return (u32)c;
}

static int png_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h){
    if(len < 8 || data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G') return -1;
    u32 pos = 8;
    u32 width = 0, height = 0;
    int bitdepth = 0, colour = 0, interlace = 0;
    u8 palette[256 * 3];
    int have_palette = 0;
    const u8 *idat = NULL;         /* runs of IDAT (may be split) are gathered below */
    u32 idat_total = 0;
    u8 *idat_buf = NULL;
    while(pos + 8 <= len){
        u32 clen = rd32be(data + pos);
        const u8 *chunk = data + pos + 8;
        if(pos + 12 + clen > len) break;
        const u8 *type = data + pos + 4;
        if(!memcmp(type, "IHDR", 4) && clen == 13){
            width = rd32be(chunk);
            height = rd32be(chunk + 4);
            bitdepth = chunk[8];
            colour = chunk[9];
            /* chunk[10] compression, chunk[11] filter: both must be 0 in practice */
            interlace = chunk[12];
        } else if(!memcmp(type, "PLTE", 4)){
            u32 n = clen / 3;
            if(n > 256) n = 256;
            memcpy(palette, chunk, n * 3);
            have_palette = 1;
        } else if(!memcmp(type, "IDAT", 4)){
            u8 *grown = kmalloc(idat_total + clen);
            if(!grown){ if(idat_buf) kfree(idat_buf); return -1; }
            if(idat_buf){ memcpy(grown, idat_buf, idat_total); kfree(idat_buf); }
            memcpy(grown + idat_total, chunk, clen);
            idat_buf = grown;
            idat_total += clen;
        } else if(!memcmp(type, "IEND", 4)) break;
        pos += 12 + clen;
    }
    if(!width || !height || !idat_buf) goto png_fail;
    if(width > IMG_MAX_SIDE || height > IMG_MAX_SIDE) goto png_fail;
    if(interlace != 0) goto png_fail;               /* Adam7 not supported */
    if(bitdepth != 1 && bitdepth != 2 && bitdepth != 4 && bitdepth != 8 && bitdepth != 16) goto png_fail;
    /* sub-8-bit colour only has meaning for grey and palette */
    if(bitdepth < 8 && colour != 0 && colour != 3) goto png_fail;
    {
        int channels;
        switch(colour){
            case 0: channels = 1; break;
            case 2: channels = 3; break;
            case 3: channels = 1; break;
            case 4: channels = 2; break;
            case 6: channels = 4; break;
            default: goto png_fail;
        }
        if(colour == 3 && !have_palette) goto png_fail;
        int bpp = (channels * bitdepth + 7) / 8;      /* filter unit: bytes per pixel */
        if(bpp < 1) bpp = 1;
        u32 stride = ((u32)width * (u32)channels * (u32)bitdepth + 7) / 8;
        u32 raw_size = (stride + 1) * height;
        if(raw_size > IMG_MAX_OUT) goto png_fail;
        u8 *raw = kmalloc(raw_size);
        if(!raw) goto png_fail;
        u32 got = inflate_all(idat_buf + 2, idat_total - 2, raw, raw_size);   /* skip zlib header */
        kfree(idat_buf); idat_buf = NULL;
        if(got != raw_size){ kfree(raw); return -1; }
        /* unfilter */
        u8 *prev = kmalloc(stride);
        if(!prev){ kfree(raw); return -1; }
        memset(prev, 0, stride);
        u8 *line = raw;
        for(u32 y = 0; y < height; y++){
            u8 ft = line[0];
            u8 *cur = line + 1;
            for(u32 x = 0; x < stride; x++){
                u8 a = x >= (u32)bpp ? cur[x - bpp] : 0;
                u8 b = prev[x];
                u8 c = x >= (u32)bpp ? prev[x - bpp] : 0;
                u8 v = cur[x];
                switch(ft){
                    case 0: break;
                    case 1: cur[x] = (u8)(v + a); break;
                    case 2: cur[x] = (u8)(v + b); break;
                    case 3: cur[x] = (u8)(v + ((a + b) >> 1)); break;
                    case 4: cur[x] = (u8)(v + paeth(a, b, c)); break;
                    default: kfree(prev); kfree(raw); return -1;
                }
            }
            memcpy(prev, cur, stride);
            line += stride + 1;
        }
        kfree(prev);
        /* to 0xRRGGBB */
        u32 *px = kmalloc((u32)width * height * 4);
        if(!px){ kfree(raw); return -1; }
        u8 *src2 = raw;
        for(u32 y = 0; y < height; y++){
            u8 *l = src2 + 1;
            for(u32 x = 0; x < width; x++){
                u32 r = 0, g = 0, b2 = 0;
                if(bitdepth == 16){
                    /* big-endian; keep the high byte */
                    if(colour == 0){ r = g = b2 = l[0]; }
                    else if(colour == 4){ r = g = b2 = l[0]; }
                    else { r = l[0]; g = l[2]; b2 = l[4]; }   /* 2 channels per sample for RGB(A) 16-bit */
                    l += bpp;
                } else if(bitdepth == 8){
                    if(colour == 0 || colour == 4){ r = g = b2 = l[0]; }
                    else if(colour == 3){ u32 i = l[0]; r = palette[i * 3]; g = palette[i * 3 + 1]; b2 = palette[i * 3 + 2]; }
                    else { r = l[0]; g = l[1]; b2 = l[2]; }
                    l += bpp;
                } else {   /* 1, 2 or 4 bits per channel, single channel (grey or palette) */
                    const u8 *bitrow = src2 + 1;
                    u32 bit_off = x * (u32)bitdepth;
                    u32 byte_i = bit_off >> 3, shift = 8 - bitdepth - (bit_off & 7);
                    u32 mask = (1u << bitdepth) - 1;
                    u32 sample = (bitrow[byte_i] >> shift) & mask;
                    if(colour == 3){
                        if(sample > 255) sample = 0;
                        r = palette[sample * 3]; g = palette[sample * 3 + 1]; b2 = palette[sample * 3 + 2];
                    } else {   /* grey: scale to 8 bits */
                        u32 v = sample * 255 / mask;
                        r = g = b2 = v;
                    }
                }
                px[y * width + x] = (r << 16) | (g << 8) | b2;
            }
            src2 += stride + 1;
        }
        kfree(raw);
        *out_px = px; *out_w = width; *out_h = height;
        return 0;
    }
png_fail:
    if(idat_buf) kfree(idat_buf);
    (void)idat;
    return -1;
}

/* ================= BMP ================= */

static int bmp_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h){
    if(len < 54 || data[0] != 'B' || data[1] != 'M') return -1;
    u32 off = rd32be(data + 10) >> 24 | 0;   /* placeholder, replaced below */
    (void)off;
    u32 pixel_off = (u32)data[10] | ((u32)data[11] << 8) | ((u32)data[12] << 16) | ((u32)data[13] << 24);
    i32 width = (i32)((u32)data[18] | ((u32)data[19] << 8) | ((u32)data[20] << 16) | ((u32)data[21] << 24));
    i32 height = (i32)((u32)data[22] | ((u32)data[23] << 8) | ((u32)data[24] << 16) | ((u32)data[25] << 24));
    u16 planes = (u16)(data[26] | (data[27] << 8));
    u16 bpp = (u16)(data[28] | (data[29] << 8));
    u32 compression = (u32)data[30] | ((u32)data[31] << 8) | ((u32)data[32] << 16) | ((u32)data[33] << 24);
    if(planes != 1) return -1;
    if(compression != 0 && !(bpp == 32 && compression == 3)) return -1;   /* only BI_RGB / 32-bit BITFIELDS */
    int top_down = height < 0;
    if(top_down) height = -height;
    if(width <= 0 || height <= 0 || width > IMG_MAX_SIDE || height > IMG_MAX_SIDE) return -1;
    u32 stride = ((u32)width * bpp + 31) / 32 * 4;
    if(pixel_off + stride * (u32)height > len + 3) {
        /* some writers pad less; be lenient about the check for the last row */
        if(pixel_off >= len) return -1;
    }
    u32 pal[256];
    if(bpp == 8){
        u32 pal_off = 54;                     /* after BITMAPINFOHEADER */
        for(int i = 0; i < 256; i++){
            if(pal_off + 4 * (u32)i + 4 > len) return -1;
            pal[i] = ((u32)data[pal_off + 4 * i + 2] << 16) | ((u32)data[pal_off + 4 * i + 1] << 8) | data[pal_off + 4 * i];
        }
    }
    u32 *px = kmalloc((u32)width * (u32)height * 4);
    if(!px) return -1;
    for(i32 y = 0; y < height; y++){
        i32 src_y = top_down ? y : height - 1 - y;
        u32 row = pixel_off + (u32)src_y * stride;
        if(row >= len){ kfree(px); return -1; }
        for(i32 x = 0; x < width; x++){
            u32 c;
            if(bpp == 24){
                u32 p = row + (u32)x * 3;
                if(p + 3 > len){ kfree(px); return -1; }
                c = ((u32)data[p + 2] << 16) | ((u32)data[p + 1] << 8) | data[p];
            } else if(bpp == 32){
                u32 p = row + (u32)x * 4;
                if(p + 4 > len){ kfree(px); return -1; }
                c = ((u32)data[p + 2] << 16) | ((u32)data[p + 1] << 8) | data[p];
            } else if(bpp == 8){
                u32 p = row + (u32)x;
                if(p >= len){ kfree(px); return -1; }
                c = pal[data[p]];
            } else { kfree(px); return -1; }
            px[y * (u32)width + x] = c;
        }
    }
    *out_px = px; *out_w = (u32)width; *out_h = (u32)height;
    return 0;
}

/* ================= GIF (LZW) ================= */

static int gif_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h){
    if(len < 13) return -1;
    if(memcmp(data, "GIF87a", 6) && memcmp(data, "GIF89a", 6)) return -1;
    u32 width = (u32)data[6] | ((u32)data[7] << 8);
    u32 height = (u32)data[8] | ((u32)data[9] << 8);
    if(!width || !height || width > IMG_MAX_SIDE || height > IMG_MAX_SIDE) return -1;
    u32 pos = 10;
    u8 packed = data[10];
    int gct = (packed & 0x80) ? (2 << (packed & 7)) : 0;
    pos = 13;                           /* width(2) height(2) packed(1) bg(1) aspect(1) */
    u8 gct_table[256 * 3];
    if(gct){
        if(pos + (u32)gct * 3 > len) return -1;
        memcpy(gct_table, data + pos, (u32)gct * 3);
        pos += (u32)gct * 3;
    }
    u32 transparent = 0xFFFFFFFFu;
    for(;;){
        if(pos >= len) return -1;
        u8 sep = data[pos++];
        if(sep == 0x3B) return -1;                       /* trailer: no image */
        if(sep == 0x21){                                 /* extension */
            if(pos >= len) return -1;
            u8 label = data[pos++];
            if(label == 0xF9 && pos + 2 <= len){         /* graphics control */
                u8 blk = data[pos];
                if(blk >= 4 && pos + 1 + (u32)blk + 1 <= len){
                    u8 gp = data[pos + 1];
                    if(gp & 1) transparent = data[pos + 2];
                }
            }
            while(pos < len){ u8 sz = data[pos++]; if(sz == 0) break; pos += sz; }
            continue;
        }
        if(sep != 0x2C) return -1;                       /* not an image separator */
        if(pos + 9 > len) return -1;
        u32 iw = (u32)data[pos + 4] | ((u32)data[pos + 5] << 8);
        u32 ih = (u32)data[pos + 6] | ((u32)data[pos + 7] << 8);
        u8 ipacked = data[pos + 8];
        pos += 9;
        const u8 *ct = NULL;
        u8 lct_table[256 * 3];
        if(ipacked & 0x80){                              /* local colour table */
            u32 n = 2u << (ipacked & 7);
            if(pos + n * 3 > len) return -1;
            memcpy(lct_table, data + pos, n * 3);
            ct = lct_table;
            pos += n * 3;
        } else if(gct) ct = gct_table;
        int interlaced = (ipacked & 0x40) != 0;
        if(!ct) return -1;
        if(iw == 0 || ih == 0 || pos >= len) return -1;
        u32 *dst_of = kmalloc(ih * 4);
        if(!dst_of) return -1;
        if(interlaced){
            static const u32 start[4] = { 0, 4, 2, 1 };
            static const u32 step[4] = { 8, 8, 4, 2 };
            u32 src = 0;
            for(int p = 0; p < 4; p++)
                for(u32 dst = start[p]; dst < ih; dst += step[p]) dst_of[src++] = dst;
        } else {
            for(u32 y = 0; y < ih; y++) dst_of[y] = y;
        }
        u8 min_code = data[pos++];
        if(min_code > 11){ kfree(dst_of); return -1; }
        /* the LZW data is a series of sub-blocks: gather them first */
        u8 *lzw = kmalloc(len - pos + 1);
        if(!lzw){ kfree(dst_of); return -1; }
        u32 lzw_n = 0;
        for(;;){
            if(pos >= len){ kfree(lzw); kfree(dst_of); return -1; }
            u8 sz = data[pos++];
            if(sz == 0) break;
            if(pos + sz > len){ kfree(lzw); kfree(dst_of); return -1; }
            memcpy(lzw + lzw_n, data + pos, sz);
            lzw_n += sz;
            pos += sz;
        }
        /* LZW decode (variable code size, early-change = 1 as per the GIF spec) */
        int clear = 1 << min_code, eoi = clear + 1;
        int code_size = min_code + 1;
        int next = eoi + 1;
        u16 *prefix = kmalloc(4096 * 2);
        u8 *suffix = kmalloc(4096);
        u8 *stack_buf = kmalloc(4096 + 1);
        u8 *out = kmalloc((u64)iw * ih + 1);
        if(!prefix || !suffix || !stack_buf || !out){
            kfree(prefix); kfree(suffix); kfree(stack_buf); kfree(out);
            kfree(lzw); kfree(dst_of); return -1;
        }
        for(int i = 0; i < clear; i++){ prefix[i] = 0xFFFF; suffix[i] = (u8)i; }
        u32 bitpos = 0, o = 0;
        int prev_code = -1, dict_reset = 1;
        u8 first_char = 0;
        while(bitpos + (u32)code_size <= lzw_n * 8){
            u32 code = 0;
            for(int i = 0; i < code_size; i++){
                u8 bb = lzw[bitpos >> 3];
                code |= (u32)((bb >> (bitpos & 7)) & 1) << i;
                bitpos++;
            }
            if((int)code == clear){ code_size = min_code + 1; next = eoi + 1; prev_code = -1; dict_reset = 1; continue; }
            if((int)code == eoi) break;
            if(dict_reset){
                if((int)code >= clear) goto gif_bad;
                if(o >= iw * ih) goto gif_bad;
                out[o++] = (u8)code;
                first_char = (u8)code;
                prev_code = (int)code;
                dict_reset = 0;
                continue;
            }
            int cur = (int)code;
            int added = 0;
            if(cur > next) goto gif_bad;
            if(cur == next){                     /* KwKwK: prev + first(prev) */
                if(prev_code < 0 || next >= 4096) goto gif_bad;
                prefix[next] = (u16)prev_code;
                suffix[next] = first_char;
                next++;
                if(next == (1 << code_size) && code_size < 12) code_size++;
                added = 1;
            }
            int c = cur, stack_n = 0;
            while(c >= clear){
                if(stack_n >= 4096) goto gif_bad;
                stack_buf[stack_n++] = suffix[c];
                int pc = prefix[c];
                if(pc == 0xFFFF || pc >= next) goto gif_bad;
                c = pc;
            }
            stack_buf[stack_n++] = (u8)c;
            first_char = (u8)c;
            while(stack_n){
                if(o >= iw * ih) goto gif_out;
                out[o++] = stack_buf[--stack_n];
            }
            if(prev_code >= 0 && !added && next < 4096){
                prefix[next] = (u16)prev_code;
                suffix[next] = first_char;
                next++;
                if(next == (1 << code_size) && code_size < 12) code_size++;
            }
            prev_code = cur;
        }
    gif_out:
        goto gif_frame;
    gif_bad:
        kfree(prefix); kfree(suffix); kfree(stack_buf); kfree(out); kfree(lzw); kfree(dst_of);
        return -1;
    gif_frame:
        ;
        /* expand the frame into the logical screen; transparent pixels take the
           screen's background colour (the GIF has no alpha channel) */
        u32 *px = kmalloc(width * height * 4);
        if(!px){ kfree(prefix); kfree(suffix); kfree(stack_buf); kfree(out); kfree(lzw); kfree(dst_of); return -1; }
        u32 bg = 0;
        if(gct){
            u32 bgidx = data[11];
            bg = ((u32)gct_table[bgidx * 3] << 16) | ((u32)gct_table[bgidx * 3 + 1] << 8) | gct_table[bgidx * 3 + 2];
        }
        for(u32 i2 = 0; i2 < width * height; i2++) px[i2] = bg;
        for(u32 y = 0; y < ih && y < height; y++){
            u32 dy = dst_of[y];
            if(dy >= height) continue;
            for(u32 x = 0; x < iw && x < width; x++){
                u32 idx = out[y * iw + x];
                if(idx == transparent) continue;         /* screen keeps black */
                const u8 *p = ct + (idx & 255) * 3;
                px[dy * width + x] = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
        }
        kfree(prefix); kfree(suffix); kfree(stack_buf); kfree(out); kfree(lzw); kfree(dst_of);
        *out_px = px; *out_w = width; *out_h = height;
        return 0;                                        /* first frame only */
    }
}

/* ================= entry point ================= */

int web_add_image(int idx, const char *data, u32 len){
    if(idx < 0 || idx >= nimages) return -1;
    image_t *im = &images[idx];
    if(im->state == 1) return 0;                        /* already decoded */
    u32 *px = NULL; u32 w = 0, h = 0;
    const u8 *d = (const u8 *)data;
    int ok = -1;
    if(len >= 8 && d[0] == 0x89 && d[1] == 'P') ok = png_decode(d, len, &px, &w, &h);
    else if(len >= 2 && d[0] == 'B' && d[1] == 'M') ok = bmp_decode(d, len, &px, &w, &h);
    else if(len >= 6 && !memcmp(d, "GIF8", 4)) ok = gif_decode(d, len, &px, &w, &h);
    if(ok != 0){ im->state = 2; return -1; }
    if(im->pixels) kfree(im->pixels);
    im->pixels = px; im->w = w; im->h = h; im->state = 1;
    return 0;
}

/* ================= data: URLs ================= */

static int b64_val(int c){
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

/* Decodes a data:[<mediatype>][;base64],<payload> URL into the image slot idx. 0 on success. */
int web_add_image_data_url(int idx, const char *url){
    if(idx < 0 || idx >= nimages) return -1;
    const char *comma = url;
    while(*comma && *comma != ',') comma++;
    if(!*comma) return -1;
    int is_b64 = 0;
    for(const char *q = url; q < comma; q++)
        if(q[0] == ';' && q[1] == 'b' && q[2] == 'a' && q[3] == 's' && q[4] == 'e' && q[5] == '6' && q[6] == '4') is_b64 = 1;
    if(!is_b64){
        /* percent-decoded raw bytes */
        u32 n = (u32)strlen(comma + 1);
        u8 *raw = kmalloc(n + 1);
        if(!raw) return -1;
        u32 k = 0;
        for(const char *q = comma + 1; *q; q++){
            if(*q == '%' && q[1] && q[2]){
                int hi = q[1], lo = q[2];
                hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
                lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
                if(hi >= 0 && lo >= 0){ raw[k++] = (u8)((hi << 4) | lo); q += 2; continue; }
            }
            raw[k++] = (u8)*q;
        }
        int r = web_add_image(idx, (const char *)raw, k);
        kfree(raw);
        return r;
    }
    u32 n = (u32)strlen(comma + 1);
    u8 *raw = kmalloc(n / 4 * 3 + 4);
    if(!raw) return -1;
    u32 k = 0;
    u32 acc = 0;
    int bits = 0;
    for(const char *q = comma + 1; *q; q++){
        if(*q == '=' || *q == ' ' || *q == '\n' || *q == '\r') continue;
        int v = b64_val((unsigned char)*q);
        if(v < 0) continue;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if(bits >= 8){ bits -= 8; raw[k++] = (u8)((acc >> bits) & 0xFF); }
    }
    int r = web_add_image(idx, (const char *)raw, k);
    kfree(raw);
    return r;
}
