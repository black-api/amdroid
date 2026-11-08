#include "amdroid/keyboard.h"
#include "amdroid/mouse.h"
#include "amdroid/types.h"

#define KBD_DATA 0x60
#define KBD_STATUS 0x64

static char keybuf[256];
static int kb_head=0, kb_tail=0;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static const char scancode_map[128] = {
    0,  27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

static int ctrl=0, alt=0;
static int e0=0;
volatile int requested_vt = -1;

void keyboard_init(void){ ctrl=0; alt=0; e0=0; requested_vt=-1; }

void keyboard_irq(void){
    /* Status bit 5 marks a byte from the aux (mouse) port. It must not be
       treated as a scancode, so hand it to the mouse parser instead. */
    u8 st = inb(KBD_STATUS);
    if(!(st & 1)) return;          /* output buffer empty: nothing to read */
    if(st & 0x20){
        mouse_feed(inb(KBD_DATA));
        return;
    }
    u8 sc = inb(KBD_DATA);
    if(sc == 0xE0){ e0=1; return; }
    int release = sc & 0x80;
    sc &= 0x7F;
    if(e0){
        e0=0;
        /* extended: ctrl/alt on right */
        if(sc==0x1D){ if(release) ctrl=0; else ctrl=1; return; }
        if(sc==0x38){ if(release) alt=0; else alt=1; return; }
        /* F keys? extended not needed */
        return;
    }
    /* modifiers */
    if(sc==0x1D){ if(release) ctrl=0; else ctrl=1; return; }
    if(sc==0x38){ if(release) alt=0; else alt=1; return; }
    if(release) return;
    /* F1-F9 */
    if(sc>=0x3B && sc<=0x43){
        int f = sc - 0x3B + 1; /* 1..9 */
        if(ctrl && alt){
            requested_vt = f; /* 1..9 */
        }
        return;
    }
    char c = (sc < 128) ? scancode_map[sc] : 0;
    if(c){
        int next = (kb_head+1)%256;
        if(next != kb_tail){ keybuf[kb_head]=c; kb_head=next; }
    }
}

int keyboard_has_input(void){ return kb_head != kb_tail; }
int keyboard_getchar(void){
    if(kb_head==kb_tail) return -1;
    char c = keybuf[kb_tail];
    kb_tail = (kb_tail+1)%256;
    return c;
}

/* Consumes the pending VT switch request (1..9), or returns -1 if none. */
int keyboard_get_requested_vt(void){
    int v = requested_vt;
    requested_vt = -1;
    return v;
}

/* Returns the pending VT switch request without consuming it. */
int keyboard_peek_requested_vt(void){
    return requested_vt;
}

/* Queues a VT switch request, e.g. when a GUI launched from the shell exits. */
void keyboard_request_vt(int vt){
    requested_vt = vt;
}
