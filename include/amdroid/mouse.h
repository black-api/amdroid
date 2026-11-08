#ifndef AMDROID_MOUSE_H
#define AMDROID_MOUSE_H
#include "amdroid/types.h"
void mouse_init(void);
int  mouse_poll(int *dx, int *dy, int *btns); /* returns 1 if new data */
void mouse_feed(u8 raw);
void mouse_inject(int dx, int dy, int btns);   /* USB mouse reports */  /* one aux-port byte (used by the keyboard IRQ) */
void mouse_disable(void); /* stop packet streaming when the GUI exits */
#endif
