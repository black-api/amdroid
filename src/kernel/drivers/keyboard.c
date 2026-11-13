#include "amdroid/keyboard.h"
#include "amdroid/mouse.h"
#include "amdroid/types.h"

#define KBD_DATA 0x60
#define KBD_STATUS 0x64

static u8 keybuf[256];          /* unsigned: KEY_* codes are 0x80..0x89 */
static int kb_head=0, kb_tail=0;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static const char scancode_map[128] = {
    0,  27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};

/* Shifted characters (US layout). */
static const char scancode_shift_map[128] = {
    0,  27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
    'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
    'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0
};


static int ctrl=0, alt=0, shift=0;
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
        /* extended: ctrl/alt on right, and the navigation keys */
        if(sc==0x1D){ if(release) ctrl=0; else ctrl=1; return; }
        if(sc==0x38){ if(release) alt=0; else alt=1; return; }
        if(release) return;
        u8 k = 0;
        switch(sc){
            case 0x48: k = KEY_UP; break;
            case 0x50: k = KEY_DOWN; break;
            case 0x4B: k = KEY_LEFT; break;
            case 0x4D: k = KEY_RIGHT; break;
            case 0x47: k = KEY_HOME; break;
            case 0x4F: k = KEY_END; break;
            case 0x53: k = KEY_DEL; break;
            case 0x49: k = KEY_PGUP; break;
            case 0x51: k = KEY_PGDN; break;
        }
        if(k){
            int next = (kb_head+1)%256;
            if(next != kb_tail){ keybuf[kb_head]=k; kb_head=next; }
        }
        return;
    }
    /* modifiers */
    if(sc==0x1D){ if(release) ctrl=0; else ctrl=1; return; }
    if(sc==0x38){ if(release) alt=0; else alt=1; return; }
    if(sc==0x2A || sc==0x36){ shift = release ? 0 : 1; return; }
    if(release) return;
    /* F1-F9 */
    if(sc>=0x3B && sc<=0x43){
        int f = sc - 0x3B + 1; /* 1..9 */
        if(ctrl && alt){
            requested_vt = f; /* 1..9 */
        }
        return;
    }
    char c = (sc < 128) ? (shift ? scancode_shift_map[sc] : scancode_map[sc]) : 0;
    if(ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);   /* Ctrl+letter: 1..26 */
    if(c){
        int next = (kb_head+1)%256;
        if(next != kb_tail){ keybuf[kb_head]=c; kb_head=next; }
    }
}

/* other keyboards (USB) register a hook that is run while we wait for keys */
void (*keyboard_poll_hook)(void);

void keyboard_inject(int c){
    int next = (kb_head+1)%256;
    if(next != kb_tail){ keybuf[kb_head]=(char)c; kb_head=next; }
}

int keyboard_has_input(void){
    if(keyboard_poll_hook) keyboard_poll_hook();
    return kb_head != kb_tail;
}
int keyboard_getchar(void){
    if(kb_head==kb_tail) return -1;
    u8 c = keybuf[kb_tail];
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
