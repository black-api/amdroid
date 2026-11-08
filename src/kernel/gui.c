#include "amdroid/gui.h"
#include "amdroid/fb.h"
#include "amdroid/mouse.h"
#include "amdroid/keyboard.h"
#include "amdroid/serial.h"
#include "amdroid/timer.h"
#include "amdroid/console.h"
#include "amdroid/font.h"
#include "amdroid/types.h"

/* ---- palette (0x00RRGGBB) ---- */
#define C_BG        0x0A0A0F
#define C_PANEL     0x14141E
#define C_PANEL2    0x1C1C2A
#define C_ACCENT    0x6C63FF
#define C_ACCENT2   0x00D4FF
#define C_TEXT      0xE8E8F0
#define C_MUTED     0x6B6B80
#define C_BORDER    0x2A2A3E
#define C_HOVER     0x2A2A44
#define C_SHADOW    0x000000

/* ---- metrics (all in pixels; layout scales with the screen) ---- */
#define TASKBAR_H   40
#define TITLE_H     28
#define MARGIN      24
#define GAP         16
#define SHADOW_OFF  4
#define ROW_H       (FONT_H + 10)
#define QA_ITEMS    4
#define SYS_LINES   5

typedef struct { u32 x, y, w, h; } rect_t;

typedef struct {
    rect_t taskbar;
    rect_t sys;                 /* "System" window, left */
    rect_t actions;             /* "Quick Actions" window, right */
    rect_t rows[QA_ITEMS];      /* clickable rows inside actions */
    rect_t status;              /* status line inside actions */
} layout_t;

static const char *qa_labels[QA_ITEMS] = {
    "1. Open Terminal", "2. File Manager", "3. Settings", "4. About"
};
static const char *qa_status[QA_ITEMS] = {
    "Switching to the text shell...",
    "File Manager is not implemented yet.",
    "Settings: theme is fixed to Black.",
    "am-droid OS - simple kernel GUI."
};

/* Arrow cursor: 'X' = outline, '.' = fill, ' ' = transparent. */
#define CUR_W 12
#define CUR_H 16
static const char cursor_bitmap[CUR_H][CUR_W+1] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "     X..X   ",
    "      XX    ",
};

/* ---- small string helpers (no libc in the kernel) ---- */
static void u32_to_str(u32 v, char *out){
    char tmp[12]; int n=0;
    if(v==0) tmp[n++]='0';
    while(v){ tmp[n++]=(char)('0'+v%10); v/=10; }
    int p=0;
    for(int i=n-1;i>=0;i--) out[p++]=tmp[i];
    out[p]=0;
}

static void str_cat(char *dst, const char *src){
    while(*dst) dst++;
    while((*dst++ = *src++)) ;
}

static void two_digits(char *out, u32 v){
    out[0] = (char)('0' + (v/10)%10);
    out[1] = (char)('0' + v%10);
    out[2] = 0;
}

/* hh:mm:ss from timer ticks (100 Hz) */
static void fmt_uptime(char *out, u64 ticks){
    u32 secs = (u32)(ticks/100);
    char a[3], b[3], c[3];
    two_digits(a, (secs/3600)%100);
    two_digits(b, (secs/60)%60);
    two_digits(c, secs%60);
    out[0]=a[0]; out[1]=a[1]; out[2]=':';
    out[3]=b[0]; out[4]=b[1]; out[5]=':';
    out[6]=c[0]; out[7]=c[1]; out[8]=0;
}

static int clampi(int v, int lo, int hi){
    return v < lo ? lo : (v > hi ? hi : v);
}

static int rect_contains(rect_t r, int x, int y){
    return x >= (int)r.x && y >= (int)r.y &&
           x < (int)(r.x + r.w) && y < (int)(r.y + r.h);
}

/* ---- layout: computed once per screen size, shared by draw and hit-test ---- */
static void layout_compute(u32 sw, u32 sh, layout_t *L){
    L->taskbar = (rect_t){ 0, sh - TASKBAR_H, sw, TASKBAR_H };

    u32 aw = 320;
    if(aw > sw/3) aw = sw/3;
    u32 ah = TITLE_H + GAP + QA_ITEMS*ROW_H + GAP + FONT_H + GAP;
    L->actions = (rect_t){ sw - MARGIN - SHADOW_OFF - aw, MARGIN, aw, ah };

    for(int i=0;i<QA_ITEMS;i++){
        L->rows[i] = (rect_t){
            L->actions.x + 8,
            L->actions.y + TITLE_H + GAP + i*ROW_H,
            aw - 16,
            ROW_H
        };
    }
    L->status = (rect_t){
        L->actions.x + 16,
        L->actions.y + ah - GAP - FONT_H,
        aw - 32,
        FONT_H
    };

    u32 sww = sw/2 > 2*MARGIN ? sw/2 - 2*MARGIN : sw/2;
    if(sww > 520) sww = 520;
    u32 shh = TITLE_H + GAP + SYS_LINES*ROW_H + GAP + FONT_H + GAP;
    L->sys = (rect_t){ MARGIN, MARGIN, sww, shh };
}

static int qa_hit(const layout_t *L, int x, int y){
    for(int i=0;i<QA_ITEMS;i++)
        if(rect_contains(L->rows[i], x, y)) return i;
    return -1;
}

/* ---- drawing ---- */
static void draw_window(rect_t r, const char *title){
    fb_fill_rect(r.x + SHADOW_OFF, r.y + SHADOW_OFF, r.w, r.h, C_SHADOW);
    fb_fill_rect(r.x, r.y, r.w, r.h, C_PANEL);
    fb_rect(r.x, r.y, r.w, r.h, C_BORDER);
    fb_fill_rect(r.x+1, r.y+1, r.w-2, TITLE_H-1, C_PANEL2);
    fb_draw_text(r.x+12, r.y+(TITLE_H-FONT_H)/2, title, C_TEXT, C_PANEL2);
    fb_fill_rect(r.x+1, r.y+TITLE_H, r.w-2, 2, C_ACCENT);
}

static void draw_sysmon(const layout_t *L, u32 sw, u32 sh, u64 ticks){
    draw_window(L->sys, "System");

    char res[24] = "", up[12];
    char wbuf[12], hbuf[12];
    u32_to_str(sw, wbuf); u32_to_str(sh, hbuf);
    str_cat(res, wbuf); str_cat(res, "x"); str_cat(res, hbuf);
    fmt_uptime(up, ticks);

    const char *labels[SYS_LINES] = { "Display", "Color", "Input", "Theme", "Uptime" };
    const char *values[SYS_LINES] = { res, "32-bit", "PS/2 mouse + keyboard", "Black / Dark", up };

    u32 cx = L->sys.x + 16;
    u32 cy = L->sys.y + TITLE_H + GAP;
    u32 vx = cx + 12*FONT_W;
    for(int i=0;i<SYS_LINES;i++){
        u32 y = cy + i*ROW_H + (ROW_H-FONT_H)/2;
        fb_draw_text(cx, y, labels[i], C_MUTED, C_PANEL);
        fb_draw_text(vx, y, values[i], i==0 ? C_ACCENT2 : C_TEXT, C_PANEL);
    }

    u32 fy = L->sys.y + L->sys.h - GAP - FONT_H;
    fb_draw_text(cx, fy, "Q quit   1-4 actions   Ctrl+Alt+2 shell", C_MUTED, C_PANEL);
}

static void draw_actions(const layout_t *L, int hover, int status){
    draw_window(L->actions, "Quick Actions");
    for(int i=0;i<QA_ITEMS;i++){
        rect_t r = L->rows[i];
        int hot = (i == hover);
        if(hot) fb_fill_rect(r.x, r.y, r.w, r.h, C_HOVER);
        fb_draw_text(r.x + 12, r.y + (r.h-FONT_H)/2, qa_labels[i],
                     hot ? C_ACCENT2 : C_TEXT, hot ? C_HOVER : C_PANEL);
    }
    if(status >= 0)
        fb_draw_text(L->status.x, L->status.y, qa_status[status], C_MUTED, C_PANEL);
}

static void draw_taskbar(const layout_t *L, u64 ticks){
    rect_t t = L->taskbar;
    fb_fill_rect(t.x, t.y, t.w, t.h, C_PANEL);
    fb_fill_rect(t.x, t.y, t.w, 2, C_ACCENT);
    u32 ty = t.y + (t.h - FONT_H)/2;

    fb_draw_text(16, ty, "am-droid", C_ACCENT2, C_PANEL);
    fb_draw_text(16 + 9*FONT_W, ty, "OS  |  Black Theme", C_MUTED, C_PANEL);

    char up[12] = "Up ";
    fmt_uptime(up+3, ticks);
    u32 len = 3 + 8;
    fb_draw_text(t.x + t.w - 16 - len*FONT_W, ty, up, C_TEXT, C_PANEL);
}

static void draw_cursor(int x, int y){
    for(int row=0; row<CUR_H; row++){
        for(int col=0; col<CUR_W; col++){
            char ch = cursor_bitmap[row][col];
            if(ch == ' ') continue;
            u32 color = (ch == 'X') ? 0x000000 : C_TEXT;
            int px = x + col, py = y + row;
            if(px >= 0 && py >= 0) fb_putpixel((u32)px, (u32)py, color);
        }
    }
}

static void draw_frame(const layout_t *L, u32 sw, u32 sh, int mx, int my,
                       int hover, int status, u64 ticks){
    fb_clear(C_BG);
    draw_sysmon(L, sw, sh, ticks);
    draw_actions(L, hover, status);
    draw_taskbar(L, ticks);
    draw_cursor(mx, my);
    fb_swap();
}

/* ---- actions ---- */
/* Returns the status index to show; sets *next_vt when the action leaves the GUI. */
static int run_action(int idx, int *next_vt){
    if(idx == 0) *next_vt = 2;   /* Open Terminal: switch to the text shell */
    return idx;
}

int gui_run(void){
    if(!fb_ready()){
        console_puts("No framebuffer available; GUI requires GRUB framebuffer.\n");
        return 2;
    }
    mouse_init();

    u32 sw = fb_width(), sh = fb_height();
    layout_t L;
    layout_compute(sw, sh, &L);

    int mx = (int)sw/2, my = (int)sh/2;
    int prev_btn = 0, hover = -1, status = -1;
    int next_vt = 0;             /* 0 = keep running, otherwise VT to switch to */

    while(!next_vt){
        int dx, dy, btns;
        if(mouse_poll(&dx, &dy, &btns)){
            mx = clampi(mx + dx, 0, (int)sw - 1);
            my = clampi(my + dy, 0, (int)sh - 1);
            if((btns & 1) && !(prev_btn & 1)){       /* left-button press edge */
                int idx = qa_hit(&L, mx, my);
                if(idx >= 0) status = run_action(idx, &next_vt);
            }
            prev_btn = btns;
        }
        hover = qa_hit(&L, mx, my);

        int rv = keyboard_get_requested_vt();
        if(rv != -1 && rv != 1) next_vt = rv;    /* Ctrl+Alt+N for another VT */
        if(keyboard_has_input()){
            int c = keyboard_getchar();
            if(c=='q' || c=='Q' || c==27) next_vt = 2;
            else if(c>='1' && c<='4') status = run_action(c-'1', &next_vt);
        }
        if(serial_has_input()){
            int c = serial_getc();
            if(c=='q' || c=='Q') next_vt = 2;
        }
        if(next_vt) break;

        draw_frame(&L, sw, sh, mx, my, hover, status, timer_ticks());
        timer_sleep(16);
    }

    mouse_disable();
    fb_clear(0x000000);
    fb_swap();
    return next_vt;
}
