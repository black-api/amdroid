#ifndef AMDROID_FBCON_H
#define AMDROID_FBCON_H
#include "amdroid/types.h"

/* A text console on the framebuffer, with scrollback. Output from the
 * console (shell, Fuss programs, kernel messages) lands here, and is drawn
 * on screen while the terminal is the active view. */
void fbcon_init(void);                 /* after the framebuffer is ready */
void fbcon_putc(char c);
void fbcon_set_active(int on);         /* 1 when the terminal owns the screen */
void fbcon_redraw(void);               /* repaint the whole screen */
void fbcon_flush(int force);           /* show pending output (rate limited unless forced) */
void fbcon_scroll_view(int lines);     /* PgUp/PgDn: positive = further back */
#endif
