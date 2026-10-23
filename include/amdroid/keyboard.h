#ifndef AMDROID_KEYBOARD_H
#define AMDROID_KEYBOARD_H
#include "amdroid/types.h"
void keyboard_init(void);
void keyboard_irq(void);
int  keyboard_getchar(void); /* -1 if none, else ASCII */
int  keyboard_has_input(void);
int  keyboard_get_requested_vt(void);
#endif
