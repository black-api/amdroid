#ifndef AMDROID_DICTATION_H
#define AMDROID_DICTATION_H
/* System dictation: Ctrl+Space (or an app's button) listens on the microphone; the second
 * press stops, Scribe writes it down, and the text is typed into the focused window as if
 * from the keyboard. The Dynamic Island shows it while it listens and writes. */
#include "amdroid/types.h"

enum { DICT_IDLE, DICT_LISTEN, DICT_WRITE, DICT_CONFIRM };
void dict_toggle(void);        /* start listening, or stop and write it down */
/* Alt+Space: a voice command. Listens until you stop talking, Needle picks the action, it
 * runs (or the Island asks first when unsure: dict_confirm(1) on Enter, 0 on Esc). */
void dict_command(void);
void dict_confirm(int yes, int *next_vt);
int  dict_is_command(void);
void dict_face(char *title, u32 tcap, char *body, u32 bcap);   /* what the Island says */
void dict_cancel(void);        /* stop without writing anything */
void dict_tick(int *next_vt);  /* from the GUI loop, every turn */
int  dict_state(void);
u32  dict_tenths(void);        /* how long it has listened, in tenths of a second */
u32  dict_level(void);         /* the latest loudness, 0..32767 */
#endif
