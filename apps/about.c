/* About: what this system is and how long it has been running. */
#include "amdroid/ui.h"
#include "amdroid/font.h"
#include "amdroid/timer.h"

/* Draws s at (b.x, y), wrapped to the body width at the given scale; returns the
 * y just below the last line. */
static u32 wrap_draw(rect_t b, u32 y, const char *s, u32 fg, u32 scale){
    u32 cpl = b.w / ((u32)FONT_W * scale);      /* chars per line */
    if(cpl < 8) cpl = 8;
    const char *p = s;
    while(*p){
        while(*p == ' ') p++;
        if(!*p) break;
        const char *ls = p;
        const char *q = p;
        u32 count = 0;
        const char *best = p;
        while(*q){
            while(*q == ' ') q++;
            if(!*q) break;
            const char *we = q;
            while(*we && *we != ' ') we++;
            u32 ws = (u32)(we - q);
            u32 cand = count ? count + 1 + ws : ws;
            if(cand > cpl) break;
            count = cand;
            best = we;
            q = we;
        }
        char line[192];
        u32 n = (u32)(best - ls);
        if(n >= sizeof line) n = sizeof line - 1;
        if(best == ls){                            /* one word longer than the line: hard cut */
            n = cpl < sizeof line ? cpl : sizeof line - 1;
            for(u32 i = 0; i < n; i++) line[i] = ls[i];
            best = ls + n;
        } else {
            for(u32 i = 0; i < n; i++) line[i] = ls[i];
        }
        line[n] = 0;
        ui_text(b.x, y, line, fg, scale);
        y += (u32)FONT_H * scale + 4 * scale;
        p = best;
    }
    return y;
}

static void a_draw(rect_t b, int focused){
    (void)focused;
    u32 y = wrap_draw(b, b.y, "am-droid", ui_accent(), 4) + 8;
    y = wrap_draw(b, y, "A small 64-bit kernel with its own GUI", C_TEXT, UI_SCALE) + 24;
    const char *feats[] = {
        "Fuss interpreter built in",
        "FAT32 read and write, ext2 read-only",
        "TCP/IP over e1000 and virtio-net",
        "Intel HDA audio, PS/2 and USB keyboards and mice",
        "Processes, signals, per-user permissions",
    };
    for(int i = 0; i < 5; i++){
        ui_fill((rect_t){ b.x, y + 12, 8, 8 }, ui_accent());
        u32 y0 = y;
        y = wrap_draw((rect_t){ b.x + 24, y0, b.w - 24, b.h }, y, feats[i], C_MUTED, UI_SCALE) + 12;
    }
    char up[12] = "", line[64] = "";
    ui_uptime(up, timer_ticks());
    ui_cat(line, "Uptime "); ui_cat(line, up);
    ui_text(b.x, b.y + b.h - 24, line, C_DIM, 1);
}

static void a_tick(void){                 /* the uptime counts seconds */
    static u64 shown;
    if(timer_ticks() / 100 != shown){ shown = timer_ticks() / 100; ui_redraw(); }
}

const app_t app_about = {
    .name = "About",
    .icon = UI_ICO_ABOUT,
    .open = NULL,
    .close = NULL,
    .draw = a_draw,
    .tick = a_tick,
};
