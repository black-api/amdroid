#include "amdroid/fb.h"
#include "amdroid/font.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include "amdroid/console.h"
#include "amdroid/pci.h"
#include <string.h>
#include <emmintrin.h>

/* Bochs display interface (QEMU -vga std, bochs-display): 16-bit index/data ports. */
#define DISPI_IDX   0x1CE
#define DISPI_DATA  0x1CF
#define DISPI_ID        0
#define DISPI_XRES      1
#define DISPI_YRES      2
#define DISPI_BPP       3
#define DISPI_ENABLE    4
#define DISPI_VIRT_W    6
#define DISPI_VIRT_H    7
#define DISPI_XOFF      8
#define DISPI_YOFF      9

static inline void io_outw(u16 p, u16 v){ __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16  io_inw(u16 p){ u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void dispi_write(u16 idx, u16 v){ io_outw(DISPI_IDX, idx); io_outw(DISPI_DATA, v); }
static u16 dispi_read(u16 idx){ io_outw(DISPI_IDX, idx); return io_inw(DISPI_DATA); }

int fb_bochs_set_mode(u32 w, u32 h, u64 *base, u32 *pitch){
    int dev = -1;
    for(int d = 0; d < 32 && dev < 0; d++){
        u32 id = pci_cfg_read32(0, (u8)d, 0, 0);
        if((id & 0xFFFF) == 0x1234 && (id >> 16) == 0x1111) dev = d;
    }
    if(dev < 0) return 0;
    if(dispi_read(DISPI_ID) < 0xB0C0) return 0;             /* not a Bochs-style card */
    u32 bar = pci_cfg_read32(0, (u8)dev, 0, 0x10);
    if(bar & 1) return 0;
    u64 b = bar & ~0xFULL;

    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (u16)w);
    dispi_write(DISPI_YRES, (u16)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, (u16)w);
    dispi_write(DISPI_VIRT_H, (u16)h);
    dispi_write(DISPI_XOFF, 0);
    dispi_write(DISPI_YOFF, 0);
    /* check the card took the size before switching the display on */
    if(dispi_read(DISPI_XRES) != (u16)w || dispi_read(DISPI_YRES) != (u16)h) return 0;
    dispi_write(DISPI_ENABLE, 0x41);                         /* enabled, linear framebuffer */
    *base = b;
    *pitch = w * 4;
    return 1;
}

static void *fb_addr = NULL;
static u32 fb_pitch = 0, fb_w = 0, fb_h = 0, fb_bpp = 0;
static u32 *backbuf = NULL;
static int ready = 0;

/* Everything is drawn into the back buffer and clipped to one rectangle, [cx0,cx1) x
 * [cy0,cy1). It is the base clip (the part of the screen being redrawn; the whole screen
 * by default) narrowed by the clip of the app that is drawing, if any. */
static u32 bx0, by0, bx1, by1;         /* base clip */
static u32 cx0, cy0, cx1, cy1;         /* what drawing is cut to */
/* rows changed since the last fb_swap, so it copies only those */
static u32 dirty_y0, dirty_y1;

static inline void touch(u32 y0, u32 y1){
    if(y0 < dirty_y0) dirty_y0 = y0;
    if(y1 > dirty_y1) dirty_y1 = y1;
}

static void cut(u32 *x0, u32 *y0, u32 *x1, u32 *y1, i64 x, i64 y, i64 w, i64 h, u32 lx0, u32 ly0, u32 lx1, u32 ly1){
    i64 ax0 = x, ay0 = y, ax1 = x + w, ay1 = y + h;
    if(ax0 < (i64)lx0) ax0 = lx0;
    if(ay0 < (i64)ly0) ay0 = ly0;
    if(ax1 > (i64)lx1) ax1 = lx1;
    if(ay1 > (i64)ly1) ay1 = ly1;
    if(ax1 <= ax0 || ay1 <= ay0){ *x0 = *x1 = *y0 = *y1 = 0; return; }
    *x0 = (u32)ax0; *y0 = (u32)ay0; *x1 = (u32)ax1; *y1 = (u32)ay1;
}

void fb_set_base_clip(u32 x, u32 y, u32 w, u32 h){
    cut(&bx0, &by0, &bx1, &by1, x, y, w, h, 0, 0, fb_w, fb_h);
    cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1;
}
void fb_clear_base_clip(void){ fb_set_base_clip(0, 0, fb_w, fb_h); }
void fb_set_clip(u32 x, u32 y, u32 w, u32 h){ cut(&cx0, &cy0, &cx1, &cy1, x, y, w, h, bx0, by0, bx1, by1); }
void fb_clear_clip(void){ cx0 = bx0; cy0 = by0; cx1 = bx1; cy1 = by1; }

/* 1 when any of the rectangle would be drawn: callers skip work that cannot show. */
int fb_clip_hits(i32 x, i32 y, i32 w, i32 h){
    return x < (i32)cx1 && y < (i32)cy1 && x + w > (i32)cx0 && y + h > (i32)cy0;
}

void fb_init(void *addr, u32 pitch, u32 w, u32 h, u32 bpp){
    fb_addr = addr; fb_pitch = pitch; fb_w = w; fb_h = h; fb_bpp = bpp;
    if(bpp != 32 || !addr){ ready = 0; return; }
    backbuf = kmalloc(w*h*4);
    if(!backbuf){ ready = 0; return; }
    ready = 1;
    fb_clear_base_clip();
    dirty_y0 = fb_h; dirty_y1 = 0;
    fb_clear(0x000000);
}

int fb_ready(void){ return ready; }
u32 fb_width(void){ return fb_w; }
u32 fb_height(void){ return fb_h; }

/* Sixteen pixels a step. */
static inline void fill_row(u32 *p, u32 n, u32 color){
    __m128i v = _mm_set1_epi32((int)color);
    for(; n >= 16; n -= 16, p += 16){
        _mm_storeu_si128((__m128i*)p, v);       _mm_storeu_si128((__m128i*)(p + 4), v);
        _mm_storeu_si128((__m128i*)(p + 8), v); _mm_storeu_si128((__m128i*)(p + 12), v);
    }
    for(; n >= 4; n -= 4, p += 4) _mm_storeu_si128((__m128i*)p, v);
    while(n--) *p++ = color;
}

void fb_putpixel(u32 x, u32 y, u32 color){
    if(!ready || x < cx0 || x >= cx1 || y < cy0 || y >= cy1) return;
    backbuf[y*fb_w + x] = color;
    touch(y, y + 1);
}

static inline u32 mix(u32 bg, u32 color, u32 a){      /* a: 0..256 */
    u32 ia = 256 - a;
    u32 rb = ((((bg & 0x00FF00FF) * ia + (color & 0x00FF00FF) * a) >> 8) & 0x00FF00FF);
    u32 g  = ((((bg & 0x0000FF00) * ia + (color & 0x0000FF00) * a) >> 8) & 0x0000FF00);
    return rb | g;
}

/* Alpha-blend 'color' over the existing back-buffer pixel. a is 0..256. */
void fb_blend(u32 x, u32 y, u32 color, u32 a){
    if(!ready || x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || a == 0) return;
    u32 *p = &backbuf[y*fb_w + x];
    *p = a >= 256 ? color : mix(*p, color, a);
    touch(y, y + 1);
}

/* Blends 'color' through a w x h coverage mask (one byte a pixel, 255 = solid) with its
 * top left at (x, y). Text and rounded corners are drawn with this, a row at a time. */
void fb_blend_mask(i32 x, i32 y, u32 w, u32 h, const u8 *mask, u32 color){
    if(!ready || !mask) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    for(u32 yy = y0; yy < y1; yy++){
        const u8 *m = mask + (u64)(yy - (u32)y) * w + (x0 - (u32)x);
        u32 *p = backbuf + (u64)yy * fb_w + x0;
        u32 n = x1 - x0;
        /* eight pixels at once where the mask is all clear or all solid, as most of a glyph is */
        for(; n >= 8; n -= 8, m += 8, p += 8){
            u64 v = *(const u64 *)m;
            if(v == 0) continue;
            if(v == ~0ULL){ fill_row(p, 8, color); continue; }
            for(int k = 0; k < 8; k++){
                u32 a = m[k];
                if(a) p[k] = a == 255 ? color : mix(p[k], color, a + (a >> 7));
            }
        }
        for(; n; n--, m++, p++){
            u32 a = *m;
            if(a) *p = a == 255 ? color : mix(*p, color, a + (a >> 7));
        }
    }
    touch(y0, y1);
}

/* Lays a w x h picture over the back buffer. Each source pixel holds its colour already
 * weighted by its coverage, and in the top byte how much of what is behind shows through
 * (255 = all of it, 0 = none). */
void fb_blit_over(i32 x, i32 y, u32 w, u32 h, const u32 *src){
    if(!ready || !src) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    for(u32 yy = y0; yy < y1; yy++){
        const u32 *s = src + (u64)(yy - (u32)y) * w + (x0 - (u32)x);
        u32 *p = backbuf + (u64)yy * fb_w + x0;
        for(u32 n = x1 - x0; n; n--, s++, p++){
            u32 t = *s >> 24;
            if(t == 255) continue;
            *p = t ? (*s & 0xFFFFFF) + mix(0, *p, t + (t >> 7)) : (*s & 0xFFFFFF);
        }
    }
    touch(y0, y1);
}

/* The same picture stretched to dw x dh (bilinear, in the weighted-colour form, so the edges
 * stay smooth). Used to magnify icons. */
void fb_blit_over_scaled(i32 x, i32 y, u32 dw, u32 dh, u32 sw, u32 sh, const u32 *src){
    if(!ready || !src || !dw || !dh) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, dw, dh, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    /* source coordinates in 16.16, stepped instead of divided for every pixel */
    i32 stx = (i32)((sw << 16) / dw), sty = (i32)((sh << 16) / dh);
    i32 maxx = (i32)((sw - 1) << 16), maxy = (i32)((sh - 1) << 16);
    for(u32 yy = y0; yy < y1; yy++){
        i32 fy = (i32)(yy - (u32)y) * sty + (sty >> 1) - 0x8000;
        if(fy < 0) fy = 0; else if(fy > maxy) fy = maxy;
        u32 sy0 = (u32)fy >> 16, ty = ((u32)fy >> 8) & 255;
        u32 sy1 = sy0 + 1 < sh ? sy0 + 1 : sy0;
        const u32 *r0 = src + (u64)sy0 * sw, *r1 = src + (u64)sy1 * sw;
        u32 *p = backbuf + (u64)yy * fb_w + x0;
        i32 fx = (i32)(x0 - (u32)x) * stx + (stx >> 1) - 0x8000;
        for(u32 xx = x0; xx < x1; xx++, p++, fx += stx){
            i32 cfx = fx < 0 ? 0 : fx > maxx ? maxx : fx;
            u32 sx0 = (u32)cfx >> 16, tx = ((u32)cfx >> 8) & 255;
            u32 sx1 = sx0 + 1 < sw ? sx0 + 1 : sx0;
            u32 w00 = (256 - tx) * (256 - ty), w10 = tx * (256 - ty), w01 = (256 - tx) * ty, w11 = tx * ty;
            u32 a = r0[sx0], b = r0[sx1], c = r1[sx0], d = r1[sx1];
            u32 t = ((a >> 24) * w00 + (b >> 24) * w10 + (c >> 24) * w01 + (d >> 24) * w11) >> 16;
            if(t >= 255) continue;
            u32 r = (((a >> 16) & 255) * w00 + ((b >> 16) & 255) * w10 + ((c >> 16) & 255) * w01 + ((d >> 16) & 255) * w11) >> 16;
            u32 g = (((a >> 8) & 255) * w00 + ((b >> 8) & 255) * w10 + ((c >> 8) & 255) * w01 + ((d >> 8) & 255) * w11) >> 16;
            u32 bl = ((a & 255) * w00 + (b & 255) * w10 + (c & 255) * w01 + (d & 255) * w11) >> 16;
            u32 col = (r << 16) | (g << 8) | bl;
            *p = t ? col + mix(0, *p, t + (t >> 7)) : col;
        }
    }
    touch(y0, y1);
}

/* Drawing can be pointed at another buffer for a while (to prepare a picture once and
 * reuse it), then back at the screen. */
static struct { u32 *buf; u32 w, h, bx0, by0, bx1, by1, cx0, cy0, cx1, cy1, d0, d1; } screen;
void fb_target(u32 *buf, u32 w, u32 h){
    screen.buf = backbuf; screen.w = fb_w; screen.h = fb_h;
    screen.bx0 = bx0; screen.by0 = by0; screen.bx1 = bx1; screen.by1 = by1;
    screen.cx0 = cx0; screen.cy0 = cy0; screen.cx1 = cx1; screen.cy1 = cy1;
    screen.d0 = dirty_y0; screen.d1 = dirty_y1;
    backbuf = buf; fb_w = w; fb_h = h;
    fb_clear_base_clip();
}
void fb_target_screen(void){
    backbuf = screen.buf; fb_w = screen.w; fb_h = screen.h;
    bx0 = screen.bx0; by0 = screen.by0; bx1 = screen.bx1; by1 = screen.by1;
    cx0 = screen.cx0; cy0 = screen.cy0; cx1 = screen.cx1; cy1 = screen.cy1;
    dirty_y0 = screen.d0; dirty_y1 = screen.d1;
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color){
    if(!ready) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    for(u32 yy = y0; yy < y1; yy++) fill_row(backbuf + (u64)yy * fb_w + x0, x1 - x0, color);
    touch(y0, y1);
}

void fb_clear(u32 color){ fb_fill_rect(0, 0, fb_w, fb_h, color); }

/* Copies rows from a set of physical pages (not necessarily contiguous) into
 * the back buffer at (x, y), honouring the clip. pitch_px is pixels per row of
 * the source region. The whole source is copied starting at its (0, 0). */
void fb_copy_region_pages(u32 x, u32 y, u32 w, u32 h, const u64 *pages, u32 pitch_px){
    if(!ready || !pages || !pitch_px) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    for(u32 yy = y0; yy < y1; yy++){
        u64 src_off = (u64)(yy - y) * pitch_px * 4 + (u64)(x0 - x) * 4;
        u64 page = src_off / 4096;
        u32 off = (u32)(src_off % 4096);
        u8 *dst = (u8 *)(backbuf + (u64)yy * fb_w + x0);
        u32 remaining = (x1 - x0) * 4;
        while(remaining){
            u32 take = 4096 - off;
            if(take > remaining) take = remaining;
            memcpy(dst, (const u8 *)(unsigned long)(pages[page] + off), take);
            dst += take; remaining -= take; page++; off = 0;
        }
    }
    touch(y0, y1);
}

/* Like fb_copy_region_pages, but the sw x sh picture is stretched to dw x dh (nearest
 * pixel). Rows are gathered into a line buffer first (a row of the picture can cross
 * pages), and a destination row that repeats the one above is copied whole. Used for
 * video: the decoder converts at the video's own size and the stretch happens here, in
 * the copy that has to be made anyway. */
void fb_scale_region_pages(i32 x, i32 y, u32 dw, u32 dh, const u64 *pages, u32 sw, u32 sh, u32 pitch_px){
    if(!ready || !pages || !pitch_px || !sw || !sh || !dw || !dh) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, dw, dh, cx0, cy0, cx1, cy1);
    if(x1 <= x0) return;
    static u32 line[4096];
    static u32 xmap[4096];
    static u32 map_sw = 0, map_dw = 0;
    if(sw > 4096 || dw > 4096) return;
    if(map_sw != sw || map_dw != dw){
        for(u32 i = 0; i < dw; i++){ u32 sx = (u32)(((u64)(2 * i + 1) * sw) / (2 * dw)); xmap[i] = sx < sw ? sx : sw - 1; }
        /* (x may be left of the clip: xmap is indexed from the picture's own left edge) */
        map_sw = sw; map_dw = dw;
    }
    i64 last_sy = -1;
    for(u32 yy = y0; yy < y1; yy++){
        u32 sy = (u32)(((u64)(2 * ((i64)yy - y) + 1) * sh) / (2 * dh));
        if(sy >= sh) sy = sh - 1;
        u32 *dst = backbuf + (u64)yy * fb_w;
        if((i64)sy == last_sy && yy > y0){ memcpy(dst + x0, dst - fb_w + x0, (x1 - x0) * 4); continue; }
        /* the source row, whole */
        u64 off = (u64)sy * pitch_px * 4;
        u32 left = sw * 4;
        u8 *lp = (u8 *)line;
        while(left){
            u64 page = off / 4096;
            u32 in = (u32)(off % 4096), take = 4096 - in;
            if(take > left) take = left;
            memcpy(lp, (const u8 *)(unsigned long)(pages[page] + in), take);
            lp += take; off += take; left -= take;
        }
        for(u32 xx = x0; xx < x1; xx++) dst[xx] = line[xmap[(i64)xx - x]];
        last_sy = sy;
    }
    touch(y0, y1);
}

void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg){
    if(!ready) return;
    if((unsigned char)c > 127) c = '?';
    for(int row=0; row<FONT_H; row++){
        unsigned char bits = font8x16[(unsigned char)c][row];
        u32 yy = y + (u32)row;
        if(yy < cy0 || yy >= cy1) continue;
        for(int col=0; col<8; col++){
            u32 xx = x + (u32)col;
            if(xx >= cx0 && xx < cx1) backbuf[yy*fb_w + xx] = (bits & (0x80>>col)) ? fg : bg;
        }
    }
    touch(y, y + FONT_H > fb_h ? fb_h : y + FONT_H);
}

void fb_draw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg){
    u32 cx = x;
    while(*s){
        if(*s=='\n'){ cx=x; y+=FONT_H; s++; continue; }
        fb_draw_char(cx, y, *s, fg, bg);
        cx += FONT_W;
        s++;
    }
}

/* Shows part of the back buffer on the screen. The hardware pitch (bytes per row)
 * may be more than w*4. */
/* Screen brightness, 256 = full. Applied while the picture is copied to the screen. */
static u32 bright = 256;
void fb_set_brightness(int percent){
    if(percent < 20) percent = 20;
    if(percent > 100) percent = 100;
    bright = (u32)percent * 256 / 100;
}

void fb_present(u32 x, u32 y, u32 w, u32 h){
    if(!ready) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, 0, 0, fb_w, fb_h);
    if(x1 <= x0) return;
    if(bright >= 256){
        for(u32 yy = y0; yy < y1; yy++)
            memcpy((u8*)fb_addr + (u64)yy * fb_pitch + (u64)x0 * 4, backbuf + (u64)yy * fb_w + x0, (size_t)(x1 - x0) * 4);
        return;
    }
    /* dimmed: every pixel is scaled on its way to the screen; the back buffer stays untouched */
    for(u32 yy = y0; yy < y1; yy++){
        const u32 *s = backbuf + (u64)yy * fb_w + x0;
        u32 *d = (u32*)((u8*)fb_addr + (u64)yy * fb_pitch) + x0;
        for(u32 n = x1 - x0; n; n--, s++, d++){
            u32 p = *s;
            *d = ((((p & 0xFF00FF) * bright) >> 8) & 0xFF00FF) | ((((p & 0x00FF00) * bright) >> 8) & 0x00FF00);
        }
    }
}

/* Shows the rows that were drawn to since the last call. */
void fb_swap(void){
    if(!ready || dirty_y1 <= dirty_y0) return;
    fb_present(0, dirty_y0, fb_w, dirty_y1 - dirty_y0);
    dirty_y0 = fb_h; dirty_y1 = 0;
}

/* Scrolls the back buffer up by dy pixel rows, filling the bottom with bg. */
void fb_scroll_up(u32 dy, u32 bg){
    if(!ready) return;
    if(dy >= fb_h){ fb_clear(bg); return; }
    memmove(backbuf, backbuf + (u64)dy * fb_w, (size_t)(fb_h - dy) * fb_w * 4);
    for(u32 y = fb_h - dy; y < fb_h; y++) fill_row(backbuf + (u64)y * fb_w, fb_w, bg);
    touch(0, fb_h);
}

/* Copies the clipped part of a screen-sized picture (fb_w * fb_h 0x00RRGGBB pixels)
 * into the back buffer. */
void fb_blit(const u32 *src){
    if(!ready || cx1 <= cx0) return;
    for(u32 y = cy0; y < cy1; y++)
        memcpy(backbuf + (u64)y * fb_w + cx0, src + (u64)y * fb_w + cx0, (size_t)(cx1 - cx0) * 4);
    touch(cy0, cy1);
}
