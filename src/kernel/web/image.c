/* Images: fetched by the browser over HTTP(S), decoded here, drawn by the layout engine.
 * Formats: PNG (zlib/DEFLATE, all colour types, no interlace), JPEG (baseline DCT,
 * Huffman, grey/YCbCr with any chroma subsampling; progressive is declined), GIF (LZW,
 * first frame, interlace handled), BMP (uncompressed 8/24/32-bit). Anything else is
 * reported as failed and the page keeps the <img> alt text instead. */
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

/* ================= JPEG (baseline DCT, Huffman) ================= */
/* Decodes baseline (SOF0/SOF1) JFIF/Exif JPEGs: the common web case. Progressive
 * JPEGs (SOF2) are declined and fall back to the <img> alt text. Integer IDCT and
 * fixed-point YCbCr, so no floating point at run time. */

typedef struct {
    u8  fast[512];          /* 9-bit fast path: code -> symbol index, or 255 */
    u8  size[257];
    u16 code[257];
    u8  values[256];
    u32 maxcode[18];
    int delta[17];
} jhuff_t;

typedef struct {
    const u8 *s, *end;      /* entropy byte stream */
    u32 code_buffer;        /* MSB-aligned bit buffer */
    int code_bits;
    u8  marker;             /* a marker that stopped the stream, else 0 */
    int nomore;
} jbits_t;

typedef struct {
    int id, h, v, tq;       /* component id, h/v sampling, quant table */
    int td, ta;             /* DC/AC Huffman tables (from the scan header) */
    int dcpred;
    int w2, h2;             /* decoded plane size (block-padded) */
    u8 *data;               /* the plane, w2*h2, one byte per sample */
} jcomp_t;

typedef struct {
    u16 quant[4][64];       /* natural order */
    jhuff_t hdc[4], hac[4];
    jcomp_t comp[4];
    int ncomp, W, H, hmax, vmax, mcux, mcuy, restart;
    jbits_t b;
} jpeg_t;

static const u8 jdezig[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
static const u32 jbmask[17] = {0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535};
static const int jbias[17]  = {0,-1,-3,-7,-15,-31,-63,-127,-255,-511,-1023,-2047,-4095,-8191,-16383,-32767,-65535};

static inline u8 jclamp(int x){ return x < 0 ? 0 : x > 255 ? 255 : (u8)x; }

static int jbuild_huff(jhuff_t *h, const u8 *count, const u8 *vals){
    int i, j, k = 0; u32 code;
    for(i = 0; i < 16; i++) for(j = 0; j < count[i]; j++){ if(k > 255) return -1; h->size[k++] = (u8)(i + 1); }
    h->size[k] = 0;
    code = 0; { int kk = 0;
    for(j = 1; j <= 16; j++){
        h->delta[j] = (int)((u32)kk - code);
        if(h->size[kk] == j){ while(h->size[kk] == j) h->code[kk++] = (u16)(code++); if(code - 1 >= (1u << j)) return -1; }
        h->maxcode[j] = code << (16 - j);
        code <<= 1;
    }
    h->maxcode[17] = 0xffffffff;
    memset(h->fast, 255, sizeof h->fast);
    for(i = 0; i < kk; i++){ int s = h->size[i]; if(s <= 9){ int c = h->code[i] << (9 - s), m = 1 << (9 - s); for(j = 0; j < m; j++) h->fast[c + j] = (u8)i; } }
    memcpy(h->values, vals, (u32)kk);
    }
    return 0;
}

static void jgrow(jbits_t *j){
    do {
        u32 b = j->nomore ? 0 : (j->s < j->end ? *j->s++ : (j->nomore = 1, 0));
        if(b == 0xff){
            int c = j->s < j->end ? *j->s++ : 0;
            while(c == 0xff) c = j->s < j->end ? *j->s++ : 0;
            if(c != 0){ j->marker = (u8)c; j->nomore = 1; }   /* keep b=0xff only if stuffed */
            if(c != 0) b = 0; /* a real marker: feed zeros from here */
        }
        if(j->nomore && b == 0 && j->marker) { j->code_bits += 8; continue; }
        j->code_buffer |= b << (24 - j->code_bits);
        j->code_bits += 8;
    } while(j->code_bits <= 24);
}

static int jdecode(jbits_t *j, const jhuff_t *h){
    if(j->code_bits < 16) jgrow(j);
    int c = (int)((j->code_buffer >> 23) & 511);
    int k = h->fast[c];
    if(k < 255){
        int s = h->size[k];
        if(s > j->code_bits) return -1;
        j->code_buffer <<= s; j->code_bits -= s;
        return h->values[k];
    }
    u32 temp = j->code_buffer >> 16;
    for(k = 10; k < 17; k++) if(temp < h->maxcode[k]) break;
    if(k == 17 || k > j->code_bits) return -1;
    c = (int)(((j->code_buffer >> (32 - k)) & jbmask[k]) + (u32)h->delta[k]);
    j->code_bits -= k; j->code_buffer <<= k;
    return h->values[c];
}

static int jreceive(jbits_t *j, int n){     /* n bits, sign-extended (JPEG coefficient) */
    if(n <= 0) return 0;
    if(j->code_bits < n) jgrow(j);
    int sgn = (i32)j->code_buffer >> 31;     /* 0 or -1 */
    u32 k = (j->code_buffer << n) | (j->code_buffer >> (32 - n));
    j->code_buffer = k & ~jbmask[n];
    k &= jbmask[n];
    j->code_bits -= n;
    return (int)k + (jbias[n] & ~sgn);
}

#define jf2f(x) ((int)((x) * 4096 + 0.5))
#define jfsh(x) ((x) * 4096)
#define JIDCT_1D(s0,s1,s2,s3,s4,s5,s6,s7) \
    int t0,t1,t2,t3,p1,p2,p3,p4,p5,x0,x1,x2,x3; \
    p2 = (s2); p3 = (s6); \
    p1 = (p2 + p3) * jf2f(0.5411961f); \
    t2 = p1 + p3 * jf2f(-1.847759065f); \
    t3 = p1 + p2 * jf2f(0.765366865f); \
    p2 = (s0); p3 = (s4); \
    t0 = jfsh(p2 + p3); t1 = jfsh(p2 - p3); \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = (s7); t1 = (s5); t2 = (s3); t3 = (s1); \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * jf2f(1.175875602f); \
    t0 = t0 * jf2f(0.298631336f); t1 = t1 * jf2f(2.053119869f); \
    t2 = t2 * jf2f(3.072711026f); t3 = t3 * jf2f(1.501321110f); \
    p1 = p5 + p1 * jf2f(-0.899976223f); p2 = p5 + p2 * jf2f(-2.562915447f); \
    p3 = p3 * jf2f(-1.961570560f); p4 = p4 * jf2f(-0.390180644f); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static void jidct(u8 *out, int stride, short *data){
    int i, val[64], *v = val; short *d = data;
    for(i = 0; i < 8; i++, d++, v++){
        if(d[8]==0 && d[16]==0 && d[24]==0 && d[32]==0 && d[40]==0 && d[48]==0 && d[56]==0){
            int dc = d[0] * 4;
            v[0]=v[8]=v[16]=v[24]=v[32]=v[40]=v[48]=v[56] = dc;
        } else {
            JIDCT_1D(d[0],d[8],d[16],d[24],d[32],d[40],d[48],d[56])
            x0+=512; x1+=512; x2+=512; x3+=512;
            v[0]=(x0+t3)>>10; v[56]=(x0-t3)>>10;
            v[8]=(x1+t2)>>10; v[48]=(x1-t2)>>10;
            v[16]=(x2+t1)>>10; v[40]=(x2-t1)>>10;
            v[24]=(x3+t0)>>10; v[32]=(x3-t0)>>10;
        }
    }
    for(i = 0, v = val; i < 8; i++, v += 8, out += stride){
        JIDCT_1D(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7])
        x0 += 65536 + (128<<17); x1 += 65536 + (128<<17);
        x2 += 65536 + (128<<17); x3 += 65536 + (128<<17);
        out[0]=jclamp((x0+t3)>>17); out[7]=jclamp((x0-t3)>>17);
        out[1]=jclamp((x1+t2)>>17); out[6]=jclamp((x1-t2)>>17);
        out[2]=jclamp((x2+t1)>>17); out[5]=jclamp((x2-t1)>>17);
        out[3]=jclamp((x3+t0)>>17); out[4]=jclamp((x3-t0)>>17);
    }
}

static int jdecode_block(jpeg_t *J, jcomp_t *c, short data[64]){
    memset(data, 0, 64 * sizeof(short));
    int t = jdecode(&J->b, &J->hdc[c->td]);
    if(t < 0) return -1;
    int diff = t ? jreceive(&J->b, t) : 0;
    c->dcpred += diff;
    u16 *q = J->quant[c->tq];
    data[0] = (short)(c->dcpred * q[0]);
    int k = 1;
    do {
        int rs = jdecode(&J->b, &J->hac[c->ta]);
        if(rs < 0) return -1;
        int s = rs & 15, r = rs >> 4;
        if(s == 0){ if(r != 15) break; k += 16; }
        else { k += r; if(k > 63) return -1; int z = jdezig[k]; data[z] = (short)(jreceive(&J->b, s) * q[z]); k++; }
    } while(k < 64);
    return 0;
}

static void jrestart(jpeg_t *J){
    if(J->b.code_bits < 24) jgrow(&J->b);
    J->b.code_buffer = 0; J->b.code_bits = 0; J->b.nomore = 0; J->b.marker = 0;
    for(int i = 0; i < J->ncomp; i++) J->comp[i].dcpred = 0;
}

static int jpeg_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h){
    if(len < 2 || data[0] != 0xFF || data[1] != 0xD8) return -1;
    jpeg_t *J = kmalloc(sizeof *J);
    if(!J) return -1;
    memset(J, 0, sizeof *J);
    const u8 *p = data + 2, *end = data + len;
    int got_sof = 0, rc = -1;

    for(;;){
        if(p >= end) goto done;
        if(*p != 0xFF){ p++; continue; }
        while(p < end && *p == 0xFF) p++;         /* skip fill bytes */
        if(p >= end) goto done;
        u8 m = *p++;
        if(m == 0xD9) goto done;                   /* EOI */
        if(m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   /* standalone */
        if(p + 2 > end) goto fail;
        int L = (p[0] << 8) | p[1];
        const u8 *seg = p + 2, *segend = p + L;
        if(segend > end || L < 2) goto fail;
        p = segend;

        if(m == 0xDB){                              /* DQT */
            while(seg < segend){
                int pq = seg[0] >> 4, tq = seg[0] & 15; seg++;
                if(tq > 3) goto fail;
                for(int i = 0; i < 64; i++){
                    int val = pq ? ((seg[0] << 8) | seg[1]) : seg[0];
                    seg += pq ? 2 : 1;
                    J->quant[tq][jdezig[i]] = (u16)val;
                }
            }
        } else if(m == 0xC4){                        /* DHT */
            while(seg < segend){
                int tc = seg[0] >> 4, th = seg[0] & 15; seg++;
                if(th > 3) goto fail;
                int total = 0; for(int i = 0; i < 16; i++) total += seg[i];
                if(seg + 16 + total > segend) goto fail;
                if(jbuild_huff(tc ? &J->hac[th] : &J->hdc[th], seg, seg + 16) != 0) goto fail;
                seg += 16 + total;
            }
        } else if(m == 0xDD){                        /* DRI */
            if(seg + 2 > segend) goto fail;
            J->restart = (seg[0] << 8) | seg[1];
        } else if(m == 0xC0 || m == 0xC1){           /* SOF0/SOF1 baseline */
            if(seg[0] != 8) goto fail;               /* 8-bit only */
            J->H = (seg[1] << 8) | seg[2];
            J->W = (seg[3] << 8) | seg[4];
            J->ncomp = seg[5];
            if(J->ncomp < 1 || J->ncomp > 4) goto fail;
            if(J->W < 1 || J->H < 1 || J->W > IMG_MAX_SIDE || J->H > IMG_MAX_SIDE) goto fail;
            const u8 *c = seg + 6;
            for(int i = 0; i < J->ncomp; i++){
                J->comp[i].id = c[0];
                J->comp[i].h  = c[1] >> 4;
                J->comp[i].v  = c[1] & 15;
                J->comp[i].tq = c[2];
                if(J->comp[i].h < 1 || J->comp[i].h > 4 || J->comp[i].v < 1 || J->comp[i].v > 4) goto fail;
                c += 3;
            }
            got_sof = 1;
        } else if(m == 0xC2){                         /* progressive: not supported */
            goto fail;
        } else if(m == 0xDA){                         /* SOS */
            if(!got_sof) goto fail;
            int ns = seg[0]; const u8 *c = seg + 1;
            for(int i = 0; i < ns; i++){
                int cs = c[0], td = c[1] >> 4, ta = c[1] & 15; c += 2;
                for(int k = 0; k < J->ncomp; k++) if(J->comp[k].id == cs){ J->comp[k].td = td; J->comp[k].ta = ta; }
            }
            /* entropy data starts at p (== segend) */
            J->b.s = p; J->b.end = end;
            J->b.code_buffer = 0; J->b.code_bits = 0; J->b.marker = 0; J->b.nomore = 0;

            J->hmax = J->vmax = 1;
            for(int i = 0; i < J->ncomp; i++){
                if(J->comp[i].h > J->hmax) J->hmax = J->comp[i].h;
                if(J->comp[i].v > J->vmax) J->vmax = J->comp[i].v;
            }
            J->mcux = (J->W + 8 * J->hmax - 1) / (8 * J->hmax);
            J->mcuy = (J->H + 8 * J->vmax - 1) / (8 * J->vmax);
            for(int i = 0; i < J->ncomp; i++){
                J->comp[i].w2 = J->mcux * J->comp[i].h * 8;
                J->comp[i].h2 = J->mcuy * J->comp[i].v * 8;
                J->comp[i].data = kmalloc((u32)J->comp[i].w2 * J->comp[i].h2);
                if(!J->comp[i].data) goto fail;
            }
            int todo = J->restart ? J->restart : 0x7fffffff;
            for(int my = 0; my < J->mcuy; my++){
                for(int mx = 0; mx < J->mcux; mx++){
                    for(int ci = 0; ci < J->ncomp; ci++){
                        jcomp_t *cp = &J->comp[ci];
                        for(int by = 0; by < cp->v; by++){
                            for(int bx = 0; bx < cp->h; bx++){
                                short blk[64];
                                if(jdecode_block(J, cp, blk) != 0) goto fail;
                                int ox = (mx * cp->h + bx) * 8, oy = (my * cp->v + by) * 8;
                                jidct(cp->data + oy * cp->w2 + ox, cp->w2, blk);
                            }
                        }
                    }
                    if(J->restart && --todo == 0 && !(mx == J->mcux - 1 && my == J->mcuy - 1)){
                        jrestart(J); todo = J->restart;
                    }
                }
            }

            /* colour convert into the output */
            u64 need = (u64)J->W * J->H * 4;
            if(need > IMG_MAX_OUT) goto fail;
            u32 *out = kmalloc((u32)need);
            if(!out) goto fail;
            jcomp_t *Y = &J->comp[0];
            for(int y = 0; y < J->H; y++){
                for(int x = 0; x < J->W; x++){
                    int yy = Y->data[(y * Y->v / J->vmax) * Y->w2 + (x * Y->h / J->hmax)];
                    int r, g, b;
                    if(J->ncomp == 1){ r = g = b = yy; }
                    else {
                        jcomp_t *Cb = &J->comp[1], *Cr = &J->comp[2];
                        int cb = Cb->data[(y * Cb->v / J->vmax) * Cb->w2 + (x * Cb->h / J->hmax)] - 128;
                        int cr = Cr->data[(y * Cr->v / J->vmax) * Cr->w2 + (x * Cr->h / J->hmax)] - 128;
                        r = yy + ((91881 * cr) >> 16);
                        g = yy - ((22554 * cb + 46802 * cr) >> 16);
                        b = yy + ((116130 * cb) >> 16);
                    }
                    out[y * J->W + x] = ((u32)jclamp(r) << 16) | ((u32)jclamp(g) << 8) | (u32)jclamp(b);
                }
            }
            *out_px = out; *out_w = (u32)J->W; *out_h = (u32)J->H;
            rc = 0;
            goto done;
        }
        /* APPn, COM, and anything else: already skipped via p = segend */
    }
done:
    for(int i = 0; i < 4; i++) if(J->comp[i].data) kfree(J->comp[i].data);
    kfree(J);
    return rc;
fail:
    for(int i = 0; i < 4; i++) if(J->comp[i].data) kfree(J->comp[i].data);
    kfree(J);
    return -1;
}

int web_add_image(int idx, const char *data, u32 len){
    if(idx < 0 || idx >= nimages) return -1;
    image_t *im = &images[idx];
    if(im->state == 1) return 0;                        /* already decoded */
    u32 *px = NULL; u32 w = 0, h = 0;
    const u8 *d = (const u8 *)data;
    int ok = -1;
    if(len >= 8 && d[0] == 0x89 && d[1] == 'P') ok = png_decode(d, len, &px, &w, &h);
    else if(len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) ok = jpeg_decode(d, len, &px, &w, &h);
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
