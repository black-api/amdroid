#ifndef AMDROID_VOICECMD_H
#define AMDROID_VOICECMD_H
#include "amdroid/types.h"
/* The calls Needle made for a voice command, put into words ("Open Notes, Volume 40%";
 * past = "Opened ...") and run. Both return how many calls there were, or 0 when any of
 * them is not a valid Am-Droid action (then nothing runs). *next_vt is set to switch console. */
int vc_describe(const char *calls, char *out, u32 cap, int past);
int vc_execute(const char *calls, int *next_vt);
#endif
