/* Clock: the time and date from the real-time clock, uptime, and a stopwatch. */
#include "amdroid/ui.h"
#include "amdroid/rtc.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"

static int running = 0;
static u64 base = 0;          /* stopwatch ticks accumulated before the last start */
static u64 started = 0;       /* tick when the current run started */

static u64 elapsed(u64 now){
    return running ? base + (now - started) : base;
}

static void put(char *out, int *p, u32 v, int digits){
    for(int i = digits - 1; i >= 0; i--){
        out[*p + i] = (char)('0' + v % 10);
        v /= 10;
    }
    *p += digits;
}

/* hh:mm:ss */
static void fmt_clock_full(char *out){
    rtc_time_t t;
    rtc_read(&t);
    int p = 0;
    put(out, &p, t.hour, 2); out[p++] = ':';
    put(out, &p, t.min, 2);  out[p++] = ':';
    put(out, &p, t.sec, 2);
    out[p] = 0;
}

/* yyyy-mm-dd */
static void fmt_date(char *out){
    rtc_time_t t;
    rtc_read(&t);
    int p = 0;
    put(out, &p, t.year, 4); out[p++] = '-';
    put(out, &p, t.month, 2); out[p++] = '-';
    put(out, &p, t.day, 2);
    out[p] = 0;
}

/* mm:ss.cc from ticks (100 per second) */
static void fmt_stop(char *out, u64 ticks){
    u32 cs = (u32)(ticks % 100), secs = (u32)(ticks / 100);
    int p = 0;
    put(out, &p, (secs / 60) % 100, 2); out[p++] = ':';
    put(out, &p, secs % 60, 2);        out[p++] = '.';
    put(out, &p, cs, 2);
    out[p] = 0;
}

static rect_t start_btn(rect_t b){ return (rect_t){ b.x, b.y + 330, 180, 44 }; }
static rect_t reset_btn(rect_t b){ return (rect_t){ b.x + 196, b.y + 330, 140, 44 }; }

static void toggle(void){
    u64 now = timer_ticks();
    if(running){ base = elapsed(now); running = 0; }
    else { started = now; running = 1; }
}
static void reset(void){
    running = 0;
    base = 0;
}

static void c_draw(rect_t b, int focused){
    (void)focused;
    char t[12], d[16], up[12], sw[16];
    fmt_clock_full(t);
    fmt_date(d);
    ui_uptime(up, timer_ticks());
    fmt_stop(sw, elapsed(timer_ticks()));

    ui_text(b.x, b.y, t, C_TEXT, 4);
    ui_text(b.x, b.y + 84, d, C_MUTED, UI_SCALE);
    char upl[24] = "Uptime ";
    ui_cat(upl, up);
    ui_text(b.x + 260, b.y + 84, upl, C_DIM, UI_SCALE);

    ui_fill((rect_t){ b.x, b.y + 150, b.w, 1 }, C_SEP);

    ui_text(b.x, b.y + 180, "Stopwatch", ui_accent(), UI_SCALE);
    ui_text(b.x, b.y + 224, sw, C_TEXT, 4);

    rect_t sb = start_btn(b), rb = reset_btn(b);
    ui_fill(sb, running ? C_CARD_HI : ui_accent());
    ui_text(sb.x + 28, sb.y + 6, running ? "Stop" : "Start", running ? C_TEXT : C_BAR, UI_SCALE);
    ui_fill(rb, C_CARD_HI);
    ui_text(rb.x + 28, rb.y + 6, "Reset", C_TEXT, UI_SCALE);
}

static void c_click(rect_t b, int x, int y){
    if(ui_contains(start_btn(b), x, y)){ toggle(); return; }
    if(ui_contains(reset_btn(b), x, y)){ reset(); return; }
}

static int c_key(int c){
    if(c == ' '){ toggle(); return 1; }
    if(c == 'r' || c == 'R'){ reset(); return 1; }
    return 0;
}

const app_t app_clock = {
    .name = "Clock",
    .icon = UI_ICO_CLOCK,
    .pinned = 0,
    .open = NULL,
    .close = NULL,
    .draw = c_draw,
    .key = c_key,
    .click = c_click,
};
