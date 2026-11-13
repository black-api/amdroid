#ifndef AMDROID_MOUSE_H
#define AMDROID_MOUSE_H
#include "amdroid/types.h"

/* What the mouse did since the last poll. Buttons: bit 0 left, 1 right, 2 middle. */
typedef struct {
    int dx, dy;          /* movement, right and down positive */
    int wheel;           /* wheel steps, positive towards the user (scroll down) */
    int btns;            /* buttons held now */
    int pressed;         /* buttons that went down, even if already let go */
} mouse_event_t;

void mouse_init(void);
int  mouse_poll(mouse_event_t *ev);            /* returns 1 if there is new data */
void mouse_feed(u8 raw);                       /* one aux-port byte (used by the keyboard IRQ) */
void mouse_inject(int dx, int dy, int btns);   /* USB mouse reports */
void mouse_disable(void);                      /* stop packet streaming when the GUI exits */
#endif
