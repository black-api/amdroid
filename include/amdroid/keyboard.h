#ifndef AMDROID_KEYBOARD_H
#define AMDROID_KEYBOARD_H
#include "amdroid/types.h"

/* Keys without a character are sent as codes 0x80..0x89. */
#define KEY_UP    0x81
#define KEY_DOWN  0x82
#define KEY_LEFT  0x83
#define KEY_RIGHT 0x84
#define KEY_HOME  0x85
#define KEY_END   0x86
#define KEY_DEL   0x87
#define KEY_PGUP  0x88
#define KEY_PGDN  0x89
void keyboard_init(void);
void keyboard_irq(void);
int  keyboard_getchar(void); /* -1 if none, else ASCII */
int  keyboard_has_input(void);
int  keyboard_get_requested_vt(void);
int  keyboard_peek_requested_vt(void);
void keyboard_request_vt(int vt);
void keyboard_inject(int c);                 /* a key from another source (USB keyboard) */
extern void (*keyboard_poll_hook)(void);      /* called while waiting for keys */
#endif
