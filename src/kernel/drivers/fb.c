#include "amdroid/fb.h"
#include "amdroid/font.h"
#include "amdroid/heap.h"
#include "amdroid/types.h"
#include "amdroid/console.h"

static void *fb_addr = NULL;
static u32 fb_pitch = 0, fb_w = 0, fb_h = 0, fb_bpp = 0;
static u32 *backbuf = NULL;
static int ready = 0;

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

static u32 rgb(u8 r, u8 g, u8 b){ return (r<<16)|(g<<8)|b; } /* stored as 0x00RRGGBB; little-endian mem = BB GG RR */

void fb_putpixel(u32 x, u32 y, u32 color){
    if(!ready || x>=fb_w || y>=fb_h) return;
    backbuf[y*fb_w + x] = color;
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color){
    if(!ready) return;
    for(u32 yy=y; yy<y+h && yy<fb_h; yy++)
        for(u32 xx=x; xx<x+w && xx<fb_w; xx++)
            backbuf[yy*fb_w+xx] = color;
}

void fb_clear(u32 color){
    if(!ready) return;
    for(u32 i=0;i<fb_w*fb_h;i++) backbuf[i]=color;
}

void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg){
    if(!ready) return;
    if((unsigned char)c > 127) c = '?';
    for(int row=0; row<FONT_H; row++){
        unsigned char bits = font8x16[(unsigned char)c][row];
        for(int col=0; col<8; col++){
            u32 color = (bits & (0x80>>col)) ? fg : bg;
            if(x+col < fb_w && y+row < fb_h)
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
        volatile u32 *row = (volatile u32*)(dst + (u32)y*fb_pitch);
        u32 *src = &backbuf[y*fb_w];
        for(u32 x=0;x<fb_w;x++) row[x] = src[x];
    }
}