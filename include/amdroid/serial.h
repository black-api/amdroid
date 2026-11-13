#ifndef AMDROID_SERIAL_H
#define AMDROID_SERIAL_H
#include "amdroid/types.h"

void serial_init(void);
void serial_putc(char c);
int  serial_getc(void); /* -1 if none */
int  serial_has_input(void);

#endif
