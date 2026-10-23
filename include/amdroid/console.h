#ifndef AMDROID_CONSOLE_H
#define AMDROID_CONSOLE_H
#include "amdroid/types.h"

void console_init(void);
void console_putc(char c);
void console_puts(const char *s);
void console_printf(const char *fmt, ...);

#endif
