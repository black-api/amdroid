#ifndef AMDROID_GUI_H
#define AMDROID_GUI_H
#include "amdroid/types.h"
/* Runs the GUI until it is left. Returns the VT to switch to:
   2 = text shell, 1..9 = that VT. */
int gui_run(void);
#endif
