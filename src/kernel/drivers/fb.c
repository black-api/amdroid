#include "amdroid/fb.h"
#include "amdroid/font.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include "amdroid/console.h"
#include "amdroid/pci.h"
#include <string.h>

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
/* optional clip rectangle set while an application draws itself: nothing may
 * paint outside of it, so text and widgets stay inside their window */
static int clip_on = 0;
static u32 gclip_x = 0, gclip_y = 0, gclip_w = 0, gclip_h = 0;

void fb_set_clip(u32 x, u32 y, u32 w, u32 h){ clip_on = 1; gclip_x = x; gclip_y = y; gclip_w = w; gclip_h = h; }
void fb_clear_clip(void){ clip_on = 0; }
static int clip_includes(u32 x, u32 y){
    return !clip_on || (x >= gclip_x && y >= gclip_y && x - gclip_x < gclip_w && y - gclip_y < gclip_h);
}

void fb_init(void *addr, u32 pitch, u32 w, u32 h, u32 bpp){
    fb_addr = addr; fb_pitch = pitch; fb_w = w; fb_h = h; fb_bpp = bpp;
    if(bpp != 32 || !addr){ ready = 0; return; }
    backbuf = kmalloc(w*h*4);
    if(!backbuf){ ready = 0; return; }
    ready = 1;
    fb_clear(0x000000);
}

int fb_ready(void){ return ready; }
u32 fb_width(void){ return fb_w; }
u32 fb_height(void){ return fb_h; }


void fb_putpixel(u32 x, u32 y, u32 color){
    if(!ready || x>=fb_w || y>=fb_h || !clip_includes(x, y)) return;
    backbuf[y*fb_w + x] = color;
}

/* Alpha-blend 'color' over the existing back-buffer pixel. a is 0..256. */
void fb_blend(u32 x, u32 y, u32 color, u32 a){
    if(!ready || x>=fb_w || y>=fb_h || !clip_includes(x, y)) return;
    if(a == 0) return;
    if(a >= 256){ backbuf[y*fb_w+x] = color; return; }
    u32 bg = backbuf[y*fb_w+x];
    u32 ia = 256 - a;
    u32 rb = ((((bg & 0x00FF00FF) * ia + (color & 0x00FF00FF) * a) >> 8) & 0x00FF00FF);
    u32 g  = ((((bg & 0x0000FF00) * ia + (color & 0x0000FF00) * a) >> 8) & 0x0000FF00);
    backbuf[y*fb_w+x] = rb | g;
}

/* Blits a 0x00RRGGBB image into the back buffer, scaled to (dw, dh), nearest neighbour.
   Only pixels inside the clip rect are drawn. */
void fb_blit_image(i32 x, i32 y, const u32 *src, u32 sw, u32 sh, u32 dw, u32 dh,
                   u32 clip_x, u32 clip_y, u32 clip_w, u32 clip_h){
    if(!ready || !src || !sw || !sh || !dw || !dh) return;
    i32 x0 = x > (i32)clip_x ? x : (i32)clip_x, y0 = y > (i32)clip_y ? y : (i32)clip_y;
    i32 x1 = x + (i32)dw < (i32)(clip_x + clip_w) ? x + (i32)dw : (i32)(clip_x + clip_w);
    i32 y1 = y + (i32)dh < (i32)(clip_y + clip_h) ? y + (i32)dh : (i32)(clip_y + clip_h);
    if(clip_on){                       /* the active clip narrows it further */
        i32 cgx1 = (i32)(gclip_x + gclip_w), cgy1 = (i32)(gclip_y + gclip_h);
        if(x0 < (i32)gclip_x) x0 = (i32)gclip_x;
        if(y0 < (i32)gclip_y) y0 = (i32)gclip_y;
        if(x1 > cgx1) x1 = cgx1;
        if(y1 > cgy1) y1 = cgy1;
    }
    if(x1 > (i32)fb_w) x1 = (i32)fb_w;
    if(y1 > (i32)fb_h) y1 = (i32)fb_h;
    for(i32 dy = y0; dy < y1; dy++){
        u32 sy = (u32)(((i64)(dy - y) * (i64)sh) / (i64)dh);
        if(sy >= sh) sy = sh - 1;
        const u32 *row = src + (u64)sy * sw;
        u32 *dst = backbuf + (u64)dy * fb_w;
        for(i32 dx = x0; dx < x1; dx++){
            u32 sx = (u32)(((i64)(dx - x) * (i64)sw) / (i64)dw);
            if(sx >= sw) sx = sw - 1;
            dst[dx] = row[sx];
        }
    }
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color){
    if(!ready) return;
    u32 x1 = x + w, y1 = y + h;
    if(clip_on){
        if(x < gclip_x) x = gclip_x;
        if(y < gclip_y) y = gclip_y;
        if(x1 > gclip_x + gclip_w) x1 = gclip_x + gclip_w;
        if(y1 > gclip_y + gclip_h) y1 = gclip_y + gclip_h;
    }
    if(x1 > fb_w) x1 = fb_w;
    if(y1 > fb_h) y1 = fb_h;
    for(u32 yy=y; yy<y1; yy++)
        for(u32 xx=x; xx<x1; xx++)
            backbuf[yy*fb_w+xx] = color;
}

void fb_clear(u32 color){
    if(!ready) return;
    for(u32 i=0;i<fb_w*fb_h;i++) backbuf[i]=color;
}

/* Copies rows from a set of physical pages (not necessarily contiguous) into
 * the back buffer at (x, y), honouring the clip. pitch_px is pixels per row of
 * the source region. The whole source is copied starting at its (0, 0). */
void fb_copy_region_pages(u32 x, u32 y, u32 w, u32 h, const u64 *pages, u32 pitch_px){
    if(!ready || !pages || !pitch_px) return;
    u32 orig_x = x, orig_y = y;
    u32 x1 = x + w, y1 = y + h;
    if(clip_on){
        if(x < gclip_x) x = gclip_x;
        if(y < gclip_y) y = gclip_y;
        if(x1 > gclip_x + gclip_w) x1 = gclip_x + gclip_w;
        if(y1 > gclip_y + gclip_h) y1 = gclip_y + gclip_h;
    }
    if(x1 > fb_w) x1 = fb_w;
    if(y1 > fb_h) y1 = fb_h;
    for(u32 yy = y; yy < y1; yy++){
        u64 src_off = (u64)(yy - orig_y) * pitch_px * 4 + (u64)(x - orig_x) * 4;
        u64 page = src_off / 4096;
        u32 off = (u32)(src_off % 4096);
        u8 *dst = (u8 *)(backbuf + (u64)yy * fb_w + x);
        u32 remaining = (x1 - x) * 4;
        while(remaining){
            u32 take = 4096 - off;
            if(take > remaining) take = remaining;
            memcpy(dst, (const u8 *)(unsigned long)(pages[page] + off), take);
            dst += take; remaining -= take; page++; off = 0;
        }
    }
}

void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg){
    if(!ready) return;
    if((unsigned char)c > 127) c = '?';
    for(int row=0; row<FONT_H; row++){
        unsigned char bits = font8x16[(unsigned char)c][row];
        for(int col=0; col<8; col++){
            u32 color = (bits & (0x80>>col)) ? fg : bg;
            if(x+col < fb_w && y+row < fb_h && clip_includes(x+col, y+row))
                backbuf[(y+row)*fb_w + (x+col)] = color;
        }
    }
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

void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 color){
    if(!ready) return;
    for(u32 i=0;i<w;i++){ fb_putpixel(x+i,y,color); fb_putpixel(x+i,y+h-1,color); }
    for(u32 i=0;i<h;i++){ fb_putpixel(x,y+i,color); fb_putpixel(x+w-1,y+i,color); }
}

void fb_swap(void){
    if(!ready) return;
    /* copy backbuf to framebuffer */
    /* honour the hardware pitch (bytes per row), which may exceed w*4 */
    u8 *dst = (u8*)fb_addr;
    for(u32 y=0;y<fb_h;y++){
        u32 *row = (u32*)(dst + (u32)y*fb_pitch);
        const u32 *src = &backbuf[y*fb_w];
        for(u32 x=0;x<fb_w;x++) row[x] = src[x];
    }
}
/* Scrolls the back buffer up by dy pixel rows, filling the bottom with bg. */
void fb_scroll_up(u32 dy, u32 bg){
    if(!ready) return;
    if(dy >= fb_h){ fb_clear(bg); return; }
    for(u32 y = 0; y + dy < fb_h; y++)
        for(u32 x = 0; x < fb_w; x++)
            backbuf[y*fb_w + x] = backbuf[(y+dy)*fb_w + x];
    for(u32 y = fb_h - dy; y < fb_h; y++)
        for(u32 x = 0; x < fb_w; x++)
            backbuf[y*fb_w + x] = bg;
}

/* Copies a whole frame (fb_w * fb_h 0x00RRGGBB pixels) into the back buffer. */
void fb_blit(const u32 *src){
    if(!ready) return;
    for(u32 i = 0; i < fb_w * fb_h; i++) backbuf[i] = src[i];
}
