#ifndef AMDROID_MOUSE_H
#define AMDROID_MOUSE_H
#include "amdroid/types.h"
void mouse_init(void);
int  mouse_poll(int *dx, int *dy, int *btns); /* returns 1 if new data */
#endif
