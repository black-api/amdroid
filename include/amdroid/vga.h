#ifndef AMDROID_VGA_H
#define AMDROID_VGA_H
#include "amdroid/types.h"

void vga_init(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);

#endif
