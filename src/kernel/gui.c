#include "amdroid/gui.h"
#include "amdroid/fb.h"
#include "amdroid/mouse.h"
#include "amdroid/keyboard.h"
#include "amdroid/serial.h"
#include "amdroid/timer.h"
#include "amdroid/console.h"
#include "amdroid/types.h"

#define C_BG     0x0A0A0F
#define C_PANEL  0x14141E
#define C_PANEL2 0x1C1C2A
#define C_ACCENT 0x6C63FF
#define C_ACCENT2 0x00D4FF
#define C_TEXT   0xE8E8F0
#define C_MUTED  0x6B6B80
#define C_BORDER 0x2A2A3E
#define C_HOVER  0x2A2A44

/* Quick Actions layout (shared by drawing and hit-testing) */
#define QA_W      300
#define QA_H      200
#define QA_ROW0   120
#define QA_ROWH   24
#define QA_ITEMS  4

static int mx=0, my=0;
static int status_msg_id = -1;   /* which action was last used, -1 = none */

static const char *qa_labels[QA_ITEMS] = {
    "1. Open Terminal", "2. File Manager", "3. Settings", "4. About"
};
static const char *qa_status[QA_ITEMS] = {
    "Leaving GUI, returning to shell...",
    "File Manager is not implemented yet.",
    "Settings: theme is fixed to Black.",
    "am-droid OS - simple kernel GUI."
};

static void draw_panel(u32 x,u32 y,u32 w,u32 h, const char *title){
    fb_fill_rect(x+4, y+4, w, h, 0x000000);      /* shadow */
    fb_fill_rect(x, y, w, h, C_PANEL);           /* body */
    fb_rect(x, y, w, h, C_BORDER);               /* border */
    fb_fill_rect(x+1, y+1, w-2, 28, C_PANEL2);   /* title bar */
    fb_draw_text(x+12, y+8, title, C_TEXT, C_PANEL2);
    fb_fill_rect(x+1, y+29, w-2, 2, C_ACCENT);   /* accent line */
}

static void u32_to_str(u32 v, char *out){
    char tmp[12]; int n=0;
    if(v==0) tmp[n++]='0';
    while(v){ tmp[n++]=(char)('0'+v%10); v/=10; }
    int p=0;
    for(int i=n-1;i>=0;i--) out[p++]=tmp[i];
    out[p]=0;
}

static void draw_taskbar(u32 w, u32 h){
    fb_fill_rect(0, h-48, w, 48, C_PANEL);
    fb_fill_rect(0, h-48, w, 2, C_ACCENT);
    fb_draw_text(16, h-32, "am-droid OS  |  Black Theme  |  High-Res GUI", C_TEXT, C_PANEL);
    char buf[32] = "Up:";
    char num[12];
    u32_to_str((u32)(timer_ticks()/100), num);   /* ticks at 100 Hz */
    int p=3;
    for(int i=0;num[i];i++) buf[p++]=num[i];
    buf[p]=0;
    fb_draw_text(w-120, h-32, buf, C_MUTED, C_PANEL);
}

static void draw_cursor(int x, int y){
    for(int i=0;i<12;i++){
        for(int j=0;j<=i && j<8;j++){
            int px=x+j, py=y+i;
            if(px>=0 && py>=0 && (u32)px<fb_width() && (u32)py<fb_height())
                fb_putpixel(px,py, C_TEXT);
        }
    }
}

/* returns index of Quick Actions item under the pointer, or -1 */
static int qa_hit(u32 w, int x, int y){
    int px0 = (int)(w-360), py0 = 80;
    if(x < px0+1 || x > px0+QA_W-2) return -1;
    for(int i=0;i<QA_ITEMS;i++){
        int top = py0 + QA_ROW0 - 80 + i*QA_ROWH - 4;
        if(y >= top && y < top + QA_ROWH) return i;
    }
    return -1;
}

static void run_action(int idx, int *running){
    status_msg_id = idx;
    if(idx == 0) *running = 0;   /* Open Terminal: leave GUI for the shell */
}

void gui_run(void){
    if(!fb_ready()){
        console_puts("No framebuffer available; GUI requires GRUB framebuffer.\n");
        return;
    }
    mouse_init();
    u32 w=fb_width(), h=fb_height();
    mx = (int)w/2; my = (int)h/2;
    int running=1;
    int prev_btn=0;
    int hover=-1;

    while(running){
        /* input */
        int dx,dy,btns;
        if(mouse_poll(&dx,&dy,&btns)){
            mx += dx; my += dy;
            if(mx<0) mx=0;
            if(my<0) my=0;
            if((u32)mx>=w) mx=w-1;
            if((u32)my>=h) my=h-1;
            /* left button press edge */
            if((btns & 1) && !(prev_btn & 1)){
                int idx = qa_hit(w, mx, my);
                if(idx >= 0) run_action(idx, &running);
            }
            prev_btn = btns;
        }
        hover = qa_hit(w, mx, my);

        int rv = keyboard_get_requested_vt();
        if(rv != -1 && rv != 1) running = 0;
        if(keyboard_has_input()){
            int c = keyboard_getchar();
            if(c=='q' || c=='Q' || c==27) running=0;
            else if(c>='1' && c<='4') run_action(c-'1', &running);
        }
        if(serial_has_input()){
            int c = serial_getc();
            if(c=='q' || c=='Q') running=0;
        }
        if(!running) break;

        /* render: flat background (no per-frame heavy effects) */
        fb_clear(C_BG);

        draw_panel(60, 60, 420, 260, "System Monitor");
        fb_draw_text(76, 100, "am-droid OS - Black Theme", C_ACCENT2, C_PANEL);
        fb_draw_text(76, 124, "Resolution:", C_MUTED, C_PANEL);
        char rbuf[32], wbuf[12], hbuf[12];
        u32_to_str(w, wbuf); u32_to_str(h, hbuf);
        int p=0;
        for(int i=0;wbuf[i];i++) rbuf[p++]=wbuf[i];
        rbuf[p++]='x';
        for(int i=0;hbuf[i];i++) rbuf[p++]=hbuf[i];
        rbuf[p]=0;
        fb_draw_text(180, 124, rbuf, C_TEXT, C_PANEL);
        fb_draw_text(76, 148, "Theme: Black / Dark", C_MUTED, C_PANEL);
        fb_draw_text(76, 172, "Input: mouse + keyboard", C_MUTED, C_PANEL);
        fb_draw_text(76, 196, "Memory: OK", C_MUTED, C_PANEL);
        fb_draw_text(76, 220, "Click an action or press Q to exit", C_ACCENT, C_PANEL);

        int px0 = (int)(w-360), py0 = 80;
        draw_panel((u32)px0, (u32)py0, QA_W, QA_H, "Quick Actions");
        for(int i=0;i<QA_ITEMS;i++){
            int top = py0 + QA_ROW0 - 80 + i*QA_ROWH - 4;
            if(i == hover)
                fb_fill_rect((u32)px0+1, (u32)top, QA_W-2, QA_ROWH, C_HOVER);
            fb_draw_text((u32)px0+24, (u32)(top+4), qa_labels[i],
                         i==hover ? C_ACCENT2 : C_TEXT,
                         i==hover ? C_HOVER : C_PANEL);
        }

        /* status line inside the Quick Actions panel */
        if(status_msg_id >= 0)
            fb_draw_text((u32)px0+12, (u32)py0+QA_H-24, qa_status[status_msg_id], C_MUTED, C_PANEL);

        draw_taskbar(w,h);
        draw_cursor(mx,my);
        fb_swap();
        timer_sleep(16);
    }
    fb_clear(0x000000);
    fb_swap();
}
