#ifndef AMDROID_FB_H
#define AMDROID_FB_H
#include "amdroid/types.h"

/* Drawing goes to a back buffer; fb_swap or fb_present puts it on the screen. */
void fb_init(void *addr, u32 pitch, u32 w, u32 h, u32 bpp);
int  fb_ready(void);
u32  fb_width(void);
u32  fb_height(void);

/* Clipping. The base clip is the part of the screen being redrawn (the GUI repaints only
 * what changed); an app's clip narrows it while the app draws, so nothing leaves its window. */
void fb_set_base_clip(u32 x, u32 y, u32 w, u32 h);
void fb_clear_base_clip(void);
void fb_set_clip(u32 x, u32 y, u32 w, u32 h);
void fb_clear_clip(void);
int  fb_clip_hits(i32 x, i32 y, i32 w, i32 h);    /* 1 when part of the rectangle would be drawn */

void fb_putpixel(u32 x, u32 y, u32 color);
/* Alpha-blend 'color' over the existing back-buffer pixel. a is 0..256 (256 = opaque). */
void fb_blend(u32 x, u32 y, u32 color, u32 a);
/* Blend 'color' through a w x h coverage mask (a byte per pixel, 255 = opaque). */
void fb_blend_mask(i32 x, i32 y, u32 w, u32 h, const u8 *mask, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
/* Lay a w x h picture over what is drawn: each pixel is a colour weighted by its coverage,
 * with the share of the background that shows through in the top byte (255 = all). */
void fb_blit_over(i32 x, i32 y, u32 w, u32 h, const u32 *src);
/* Draw into another buffer of w x h pixels for a while, then into the screen again. */
void fb_target(u32 *buf, u32 w, u32 h);
void fb_target_screen(void);
/* Copies rows from physical pages (not contiguous) into the back buffer. */
void fb_copy_region_pages(u32 x, u32 y, u32 w, u32 h, const u64 *pages, u32 pitch_px);
void fb_clear(u32 color);
void fb_draw_char(u32 x, u32 y, char c, u32 fg, u32 bg);
void fb_draw_text(u32 x, u32 y, const char *s, u32 fg, u32 bg);
void fb_blit(const u32 *src);        /* the clipped part of a screen-sized picture */
void fb_scroll_up(u32 dy, u32 bg);   /* scroll the back buffer up by dy rows */

void fb_swap(void);                              /* show the rows drawn since the last swap */
void fb_present(u32 x, u32 y, u32 w, u32 h);     /* show one rectangle */

/* Bochs/QEMU VGA only: sets a linear mode of w x h x 32 and returns its base and pitch.
   Returns 0 (and changes nothing) when the card is not present or refuses the mode. */
int fb_bochs_set_mode(u32 w, u32 h, u64 *base, u32 *pitch);

#endif
