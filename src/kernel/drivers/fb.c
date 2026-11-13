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
void fb_present(u32 x, u32 y, u32 w, u32 h){
    if(!ready) return;
    u32 x0, y0, x1, y1;
    cut(&x0, &y0, &x1, &y1, x, y, w, h, 0, 0, fb_w, fb_h);
    if(x1 <= x0) return;
    for(u32 yy = y0; yy < y1; yy++)
        memcpy((u8*)fb_addr + (u64)yy * fb_pitch + (u64)x0 * 4, backbuf + (u64)yy * fb_w + x0, (size_t)(x1 - x0) * 4);
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
