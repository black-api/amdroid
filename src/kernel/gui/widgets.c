/* Desktop widgets: cards that sit on the wallpaper under the windows. Clock, System,
 * Calendar, Network and Notes. Drag one to move it (right-click it to remove it); which ones
 * show, and their places, are kept in the preferences. */
#include "gui_internal.h"
#include "amdroid/net.h"
#include "amdroid/rtc.h"
#include "amdroid/pmm.h"
#include "amdroid/fat32.h"
#include "amdroid/timer.h"
#include "amdroid/fb.h"

#define W_W 300
#define MARGIN 24
#define GAP 16
static const int W_H[UI_NWIDGETS] = { 130, 150, 292, 118, 190 };
static const char *W_NAME[UI_NWIDGETS] = { "Clock", "System", "Calendar", "Network", "Notes" };

static int scr_w, scr_h;
static int zorder[UI_NWIDGETS];                       /* back to front */
static int drag_w = -1, grab_x, grab_y;
static rect_t shown[UI_NWIDGETS];                     /* where each was last drawn (w = 0: not shown) */
static u32 shown_sig[UI_NWIDGETS];

void wid_init(u32 sw, u32 sh){
    scr_w = (int)sw; scr_h = (int)sh;
    for(int i = 0; i < UI_NWIDGETS; i++) zorder[i] = i;
}

static int on(int w){ return ui_prefs.wid_on && (ui_prefs.wid_mask & (1 << w)); }

/* Where a widget is: its own place once it has been dragged, else a slot in a column down
 * the right side. */
static rect_t place(int w){
    int x = ui_prefs.wid_x[w], y = ui_prefs.wid_y[w];
    if(x < 0 || y < 0){
        x = scr_w - W_W - MARGIN - dk_reserve_right();
        y = 76;
        for(int i = 0; i < w; i++)
            if(on(i) && ui_prefs.wid_x[i] < 0) y += W_H[i] + GAP;
        if(y + W_H[w] > scr_h - 90) y = scr_h - 90 - W_H[w];     /* no room: it overlaps rather than leaves */
        if(y < 8) y = 8;
    }
    if(x > scr_w - W_W - 4) x = scr_w - W_W - 4;
    if(x < 4) x = 4;
    if(y > scr_h - W_H[w] - 4) y = scr_h - W_H[w] - 4;
    if(y < 4) y = 4;
    return (rect_t){ (u32)x, (u32)y, W_W, (u32)W_H[w] };
}

int wid_at(int x, int y){
    for(int i = UI_NWIDGETS - 1; i >= 0; i--){
        int w = zorder[i];
        if(on(w) && ui_contains(place(w), x, y)) return w;
    }
    return -1;
}

void wid_press(int w, int x, int y){
    rect_t r = place(w);
    drag_w = w; grab_x = x - (int)r.x; grab_y = y - (int)r.y;
    for(int i = 0; i < UI_NWIDGETS; i++) if(zorder[i] == w){      /* raised */
        for(int k = i; k < UI_NWIDGETS - 1; k++) zorder[k] = zorder[k + 1];
        zorder[UI_NWIDGETS - 1] = w;
        break;
    }
}
int wid_dragging(void){ return drag_w >= 0; }

void wid_update(int mx, int my, int held){
    if(drag_w < 0) return;
    if(!(held & 1)){
        drag_w = -1;
        prefs_touch();
        return;
    }
    int x = mx - grab_x, y = my - grab_y;
    /* near the screen edges a widget sits against them */
    if(x < 12) x = 12;
    if(x > scr_w - W_W - 12) x = scr_w - W_W - 12;
    if(y < 12) y = 12;
    if(y > scr_h - W_H[drag_w] - 70) y = scr_h - W_H[drag_w] - 70;      /* above the taskbar */
    ui_prefs.wid_x[drag_w] = x;
    ui_prefs.wid_y[drag_w] = y;
}

void wid_remove(int w){
    ui_prefs.wid_mask &= ~(1 << w);
    ui_prefs_changed();
}
void wid_reset_positions(void){
    for(int i = 0; i < UI_NWIDGETS; i++) ui_prefs.wid_x[i] = ui_prefs.wid_y[i] = -1;
    ui_prefs_changed();
}

/* ---- what the cards show ---- */
static char note_text[512];
static int note_len = 0;
static u64 note_read_at = 0;
static int note_loaded = 0;

static void notes_refresh(u64 now){
    if(note_loaded && now - note_read_at < 3000) return;          /* every 30 s */
    note_read_at = now; note_loaded = 1;
    u32 size = 0;
    if(fat32_read_path("NOTES.MD", note_text, sizeof note_text - 1, &size) != 0) { note_len = 0; note_text[0] = 0; return; }
    note_len = (int)(size < sizeof note_text - 1 ? size : sizeof note_text - 1);
    note_text[note_len] = 0;
}

static u32 hash(const void *p, u32 n){
    const u8 *b = p; u32 h = 2166136261u;
    for(u32 i = 0; i < n; i++){ h ^= b[i]; h *= 16777619u; }
    return h;
}

/* A number that changes whenever what the widget shows changes. */
static u32 signature(int w, u64 now){
    rtc_time_t t; rtc_read(&t);
    switch(w){
        case WID_CLOCK: return t.hour * 100u + t.min + ((u32)t.day << 16) + ((u32)ui_accent_index() << 24);
        case WID_SYSTEM: return tb_cpu_tenths() + (tb_ram_pct() << 12) + ((u32)(now / 100) << 20);
        case WID_CALENDAR: return (u32)t.day + ((u32)t.month << 8) + ((u32)t.year << 16) + ((u32)ui_accent_index() << 28);
        case WID_NETWORK: return (u32)net_up() + ((u32)(now / 500) << 1);
        case WID_NOTES: notes_refresh(now); return hash(note_text, (u32)note_len);
    }
    return 0;
}

void wid_damage(u64 now){
    for(int w = 0; w < UI_NWIDGETS; w++){
        int shown_now = on(w);
        rect_t r = shown_now ? place(w) : (rect_t){ 0, 0, 0, 0 };
        u32 sig = shown_now ? signature(w, now) : 0;
        int moved = r.x != shown[w].x || r.y != shown[w].y || r.w != shown[w].w;
        if(moved || sig != shown_sig[w]){
            if(shown[w].w) gui_damage((int)shown[w].x - 4, (int)shown[w].y - 2, (int)shown[w].w + 12, (int)shown[w].h + 12);
            if(r.w) gui_damage((int)r.x - 4, (int)r.y - 2, (int)r.w + 12, (int)r.h + 12);
            shown[w] = r; shown_sig[w] = sig;
        }
    }
}

static void card(rect_t r, const char *title){
    ui_rrect(r.x + 2, r.y + 5, r.w, r.h, 24, C_SHADOW);
    ui_rrect(r.x, r.y, r.w, r.h, 24, C_BAR_EDGE);
    ui_rrect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 23, C_BAR);
    ui_text(r.x + 20, r.y + 14, title, ui_accent(), 1);
}

static void bar(u32 x, u32 y, u32 w, u32 pct, u32 col){
    ui_rrect(x, y, w, 10, 5, C_CARD_HI);
    u32 f = w * pct / 100;
    if(f >= 10) ui_rrect(x, y, f, 10, 5, col);
    else if(f > 0) ui_rrect(x, y, 10, 10, 5, col);
}

static void draw_clock(rect_t r){
    char c[8], d[32];
    ui_clock(c);
    ui_date_text(d, sizeof d);
    card(r, "CLOCK");
    ui_text(r.x + 20, r.y + 38, c, C_TEXT, 4);
    ui_text(r.x + 22, r.y + 94, d, C_MUTED, UI_SCALE);
}

static void draw_system(rect_t r, u64 now){
    card(r, "SYSTEM");
    char s[24], n[12], up[12];
    u32 cpu = tb_cpu_tenths(), ram = tb_ram_pct();
    ui_num(cpu / 10, n); ui_copy(s, "CPU ", sizeof s); ui_cat(s, n); ui_cat(s, "."); ui_num(cpu % 10, n); ui_cat(s, n); ui_cat(s, "%");
    ui_text(r.x + 20, r.y + 40, s, C_TEXT, 1);
    bar(r.x + 20, r.y + 60, r.w - 40, cpu / 10, ui_accent());
    ui_num(ram, n); ui_copy(s, "RAM ", sizeof s); ui_cat(s, n); ui_cat(s, "%");
    ui_text(r.x + 20, r.y + 80, s, C_TEXT, 1);
    bar(r.x + 20, r.y + 100, r.w - 40, ram, C_GOOD);
    ui_uptime(up, now);
    ui_copy(s, "Up ", sizeof s); ui_cat(s, up);
    ui_text(r.x + 20, r.y + 122, s, C_MUTED, 1);
}

static int days_in(int y, int m){
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if(m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}
static int weekday0(int y, int m, int d){
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if(m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void draw_calendar(rect_t r){
    static const char *MN[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                  "September", "October", "November", "December" };
    rtc_time_t t; rtc_read(&t);
    if(t.month < 1 || t.month > 12) t.month = 1;
    card(r, "CALENDAR");
    char title[32], n[12];
    ui_copy(title, MN[t.month - 1], sizeof title); ui_cat(title, " "); ui_num(t.year, n); ui_cat(title, n);
    ui_text(r.x + 20, r.y + 36, title, C_TEXT, UI_SCALE);
    static const char *WD[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    u32 cw = (r.w - 24) / 7;
    for(int i = 0; i < 7; i++)
        ui_text(r.x + 12 + (u32)i * cw + (cw - 16) / 2, r.y + 78, WD[i], C_DIM, 1);
    int first = weekday0(t.year, t.month, 1), dim = days_in(t.year, t.month);
    for(int d = 1; d <= dim; d++){
        int idx = first + d - 1, col = idx % 7, row = idx / 7;
        u32 cx = r.x + 12 + (u32)col * cw + cw / 2, cy = r.y + 112 + (u32)row * 28;
        char ds[4]; ui_num((u32)d, ds);
        u32 tw = ui_text_w(ds, 1);
        if(d == t.day){
            ui_disc((int)cx, (int)cy, 13, ui_accent());
            ui_text(cx - tw / 2, cy - 8, ds, 0x0B0F12, 1);
        } else ui_text(cx - tw / 2, cy - 8, ds, col == 0 || col == 6 ? C_MUTED : C_TEXT, 1);
    }
}

static void draw_network(rect_t r){
    card(r, "NETWORK");
    if(net_up()){
        u32 ip = 0, gw = 0; u8 mac[6];
        net_info(&ip, &gw, mac);
        char s[24], l[40];
        ui_disc((int)r.x + 28, (int)r.y + 52, 5, C_GOOD);
        ui_text(r.x + 42, r.y + 44, "Connected", C_TEXT, 1);
        ui_ip(s, ip); ui_copy(l, "IP  ", sizeof l); ui_cat(l, s);
        ui_text(r.x + 20, r.y + 68, l, C_MUTED, 1);
        ui_ip(s, gw); ui_copy(l, "GW  ", sizeof l); ui_cat(l, s);
        ui_text(r.x + 20, r.y + 88, l, C_MUTED, 1);
    } else {
        ui_disc((int)r.x + 28, (int)r.y + 52, 5, C_WARN);
        ui_text(r.x + 42, r.y + 44, "Not connected", C_TEXT, 1);
    }
}

static void draw_notes(rect_t r){
    card(r, "NOTES");
    if(!note_len){
        ui_text(r.x + 20, r.y + 44, "No note yet.", C_MUTED, 1);
        ui_text(r.x + 20, r.y + 64, "Write one in Notes.", C_DIM, 1);
        return;
    }
    int line = 0, i = 0;
    while(i < note_len && line < 8){
        char buf[40]; int n = 0;
        while(i < note_len && note_text[i] != '\n'){
            if(note_text[i] != '\r' && n < 34) buf[n++] = note_text[i];
            i++;
        }
        i++;
        buf[n] = 0;
        char *s = buf;
        while(*s == '#' || *s == '*' || *s == ' ') s++;           /* Markdown marks are not shown */
        if(!*s) continue;
        ui_text(r.x + 20, r.y + 40 + (u32)line * 18, s, line == 0 ? C_TEXT : C_MUTED, 1);
        line++;
    }
}

void wid_draw(void){
    if(!ui_prefs.wid_on) return;
    u64 now = timer_ticks();
    for(int i = 0; i < UI_NWIDGETS; i++){
        int w = zorder[i];
        if(!on(w)) continue;
        rect_t r = place(w);
        if(!fb_clip_hits((i32)r.x - 4, (i32)r.y - 2, (i32)r.w + 12, (i32)r.h + 12)) continue;
        switch(w){
            case WID_CLOCK: draw_clock(r); break;
            case WID_SYSTEM: draw_system(r, now); break;
            case WID_CALENDAR: draw_calendar(r); break;
            case WID_NETWORK: draw_network(r); break;
            case WID_NOTES: draw_notes(r); break;
        }
    }
}

const char *wid_name(int w){ return W_NAME[w]; }
