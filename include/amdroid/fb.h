#ifndef AMDROID_FB_H
#define AMDROID_FB_H
#include "amdroid/types.h"

void fb_init(void *addr, u32 pitch, u32 w, u32 h, u32 bpp);
int  fb_ready(void);
u32  fb_width(void);
u32  fb_height(void);
void fb_putpixel(u32 x, u32 y, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_clear(u32 color);
void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg);
void fb_draw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);
void fb_swap(void); /* if double buffered */
void fb_rect(u32 x,u32 y,u32 w,u32 h,u32 color); /* outline */

#endif
