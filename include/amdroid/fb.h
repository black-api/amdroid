#ifndef AMDROID_FB_H
#define AMDROID_FB_H
#include "amdroid/types.h"

void fb_init(void *addr, u32 pitch, u32 w, u32 h, u32 bpp);
int  fb_ready(void);
u32  fb_width(void);
u32  fb_height(void);
void fb_putpixel(u32 x, u32 y, u32 color);
/* Alpha-blend 'color' over the existing back-buffer pixel. a is 0..256 (256 = opaque). */
void fb_blend(u32 x, u32 y, u32 color, u32 a);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_clear(u32 color);
void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg);
void fb_draw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);
void fb_swap(void); /* if double buffered */
void fb_blit(const u32 *src); /* copies a full w*h frame into the back buffer */
void fb_rect(u32 x,u32 y,u32 w,u32 h,u32 color); /* outline */
/* Bochs/QEMU VGA only: sets a linear mode of w x h x 32 and returns its base and pitch.
   Returns 0 (and changes nothing) when the card is not present or refuses the mode. */
int fb_bochs_set_mode(u32 w, u32 h, u64 *base, u32 *pitch);

#endif
void fb_scroll_up(u32 dy, u32 bg); /* scroll the back buffer up by dy rows */
