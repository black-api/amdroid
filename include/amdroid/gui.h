#ifndef AMDROID_GUI_H
#define AMDROID_GUI_H
#include "amdroid/types.h"
/* Runs the GUI until it is left. Returns the VT to switch to:
   2 = text shell, 1..9 = that VT, 3 = sign out (show the login screen). */
int gui_run(void);

/* The login screen: lists the accounts, signs one in with no password. 0 when signed in. */
int gui_login(void);
#endif
