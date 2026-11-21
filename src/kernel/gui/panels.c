/* Two slide-up panels above the taskbar, on the right:
 *  - Control Center: quick tiles (Network, Do Not Disturb, Animations, Dock, Widgets, Island),
 *    volume and brightness sliders, accent colour, next wallpaper and app shortcuts.
 *  - Notification Center: the time and date, Do Not Disturb, and every notification so far,
 *    with Clear all. Clicking one opens its app and removes it; the cross dismisses it.
 * Only one is open at a time. */
#include "gui_internal.h"
#include "amdroid/net.h"
#include "amdroid/rtc.h"
#include "amdroid/timer.h"
#include "amdroid/fb.h"

#define PN_W 420
#define PN_H 600
#define SLIDE_TICKS 12
#define SLIDE_PX 40

/* Item ids (what the pointer can be over). */
enum {
    T_NET, T_DND, T_ANIM, T_DOCK, T_WIDGETS, T_ISLAND,          /* 0..5: tiles */
    S_VOL = 10, S_BRI = 11,
    ACC0 = 20, WALL = 24,
    APP0 = 30,                                                  /* 30..35: shortcuts */
    N_DND = 40, N_CLEAR = 41,
    N_ENTRY = 50,                                               /* 50..: visible rows */
    N_X = 80                                                    /* 80..: their dismiss crosses */
};
#define NSHORT 6
#define NC_ROW 68
#define NC_TOP 176

static int which = PN_NONE;
static u64 opened_at = 0;
static int hover = -1, drag = -1;
static int scroll = 0;                          /* Notification Center: rows scrolled away */
static u64 closed_at = ~0ull; static int closed_which = PN_NONE;

int pn_which(void){ return which; }

void pn_close(void){ which = PN_NONE; drag = -1; }

void pn_toggle(int w, u64 now){
    if(closed_at == now && closed_which == w) return;      /* the press that closed it also hit its button */
    if(which == w){ pn_close(); return; }
    which = w; opened_at = now; hover = -1; drag = -1; scroll = 0;
    isl_close();
}

/* A click outside the open panel closes it. */
void pn_close_outside(u64 now){
    closed_at = now; closed_which = which;
    pn_close();
}

static rect_t pn_rect(rect_t bar, u32 sw){
    int y = (int)bar.y - 14 - PN_H;
    if(y < 8) y = 8;
    return (rect_t){ sw - PN_W - 14, (u32)y, PN_W, PN_H };
}

int pn_hit(rect_t bar, u32 sw, int x, int y){
    if(!which) return 0;
    return ui_contains(pn_rect(bar, sw), x, y);
}

static int slide_dy(u64 now){
    if(!ui_anim_on || now - opened_at >= SLIDE_TICKS) return 0;
    int t = (int)((now - opened_at) * 255 / SLIDE_TICKS);
    return (255 - ui_ease(t)) * SLIDE_PX / 255;
}

static int app_by_icon(int icon){
    for(int a = 0; a < ui_app_count(); a++) if(ui_apps[a]->icon == icon) return a;
    return -1;
}
static const int SHORT_ICONS[NSHORT] = { UI_ICO_SETTINGS, UI_ICO_TASKS, UI_ICO_NOTES, UI_ICO_CALC, UI_ICO_BROWSER, UI_ICO_TERM };
/* The app a shortcut opens: an index, UI_MAX_APPS for the Terminal, -1 when the app is not there. */
static int short_app(int i){
    if(SHORT_ICONS[i] == UI_ICO_TERM) return UI_MAX_APPS;
    return app_by_icon(SHORT_ICONS[i]);
}

/* ---- the numbers on the panels ---- */
static int weekday(int y, int m, int d){                 /* 0 = Sunday */
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if(m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static const char *DAYS[7]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void ui_date_text(char *out, u32 cap){
    rtc_time_t t; rtc_read(&t);
    char n[12], s[32] = "";
    if(t.month < 1 || t.month > 12) t.month = 1;
    ui_cat(s, DAYS[weekday(t.year, t.month, t.day)]); ui_cat(s, " ");
    ui_num(t.day, n); ui_cat(s, n); ui_cat(s, " ");
    ui_cat(s, MONTHS[t.month - 1]); ui_cat(s, " ");
    ui_num(t.year, n); ui_cat(s, n);
    ui_copy(out, s, cap);
}

/* "now", "3m", "2h" ... for a notification that arrived at 'at'. */
static void age_text(char *out, u64 at, u64 now){
    u64 s = now > at ? (now - at) / 100 : 0;
    char n[12];
    if(s < 60){ ui_copy(out, "now", 8); return; }
    if(s < 3600){ ui_num((u32)(s / 60), n); ui_copy(out, n, 8); ui_cat(out, "m"); return; }
    ui_num((u32)(s / 3600), n); ui_copy(out, n, 8); ui_cat(out, "h");
}

/* ---- Control Center layout: one function says where everything is, for drawing and hitting ---- */
static rect_t cc_item(int id, rect_t R){
    if(id >= T_NET && id <= T_ISLAND){
        int c = id % 2, r = id / 2;
        return (rect_t){ R.x + 20 + (u32)c * 196, R.y + 64 + (u32)r * 80, 184, 68 };
    }
    if(id == S_VOL) return (rect_t){ R.x + 20, R.y + 338, 380, 28 };
    if(id == S_BRI) return (rect_t){ R.x + 20, R.y + 398, 380, 28 };
    if(id >= ACC0 && id < ACC0 + UI_NACCENT) return (rect_t){ R.x + 20 + (u32)(id - ACC0) * 44, R.y + 448, 36, 36 };
    if(id == WALL) return (rect_t){ R.x + R.w - 20 - 176, R.y + 444, 176, 44 };
    if(id >= APP0 && id < APP0 + NSHORT) return (rect_t){ R.x + 20 + (u32)(id - APP0) * 64, R.y + 504, 56, 56 };
    return (rect_t){ 0, 0, 0, 0 };
}
static rect_t nc_item(int id, rect_t R){
    if(id == N_DND) return (rect_t){ R.x + 20, R.y + 128, 220, 36 };
    if(id == N_CLEAR) return (rect_t){ R.x + R.w - 20 - 128, R.y + 128, 128, 36 };
    if(id >= N_ENTRY && id < N_ENTRY + 20) return (rect_t){ R.x + 12, R.y + NC_TOP + (u32)(id - N_ENTRY) * NC_ROW, R.w - 24, 64 };
    if(id >= N_X && id < N_X + 20){
        rect_t e = nc_item(id - N_X + N_ENTRY, R);
        return (rect_t){ e.x + e.w - 40, e.y + 4, 32, 32 };
    }
    return (rect_t){ 0, 0, 0, 0 };
}
static int nc_visible(void){ return (PN_H - NC_TOP - 12) / NC_ROW; }
static int nc_max_scroll(void){ int m = ui_hist_count() - nc_visible(); return m < 0 ? 0 : m; }

static int item_at(rect_t R, int x, int y){
    if(which == PN_CC){
        static const int ids[] = { T_NET, T_DND, T_ANIM, T_DOCK, T_WIDGETS, T_ISLAND, S_VOL, S_BRI, ACC0, ACC0 + 1, ACC0 + 2, ACC0 + 3,
                                   WALL, APP0, APP0 + 1, APP0 + 2, APP0 + 3, APP0 + 4, APP0 + 5 };
        for(unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
            if(ui_contains(cc_item(ids[i], R), x, y)) return ids[i];
    } else if(which == PN_NC){
        if(ui_contains(nc_item(N_DND, R), x, y)) return N_DND;
        if(ui_hist_count() && ui_contains(nc_item(N_CLEAR, R), x, y)) return N_CLEAR;
        int vis = nc_visible(), n = ui_hist_count() - scroll;
        if(n > vis) n = vis;
        for(int i = 0; i < n; i++){
            if(ui_contains(nc_item(N_X + i, R), x, y)) return N_X + i;
            if(ui_contains(nc_item(N_ENTRY + i, R), x, y)) return N_ENTRY + i;
        }
    }
    return -1;
}

static void set_slider(int id, rect_t R, int mx){
    rect_t s = cc_item(id, R);
    int lo = (int)s.x + 12, hi = (int)(s.x + s.w) - 12;
    int v = (mx - lo) * 100 / (hi - lo);
    if(v < 0) v = 0;
    if(v > 100) v = 100;
    if(id == S_VOL) ui_prefs.volume = v;
    else ui_prefs.brightness = 20 + v * 80 / 100;
    ui_prefs_live();
}

/* A press inside the panel. Returns an app to open (UI_MAX_APPS = Terminal), -1 when handled. */
int pn_press(rect_t bar, u32 sw, int x, int y, u64 now){
    (void)now;
    rect_t R = pn_rect(bar, sw);
    int id = item_at(R, x, y);
    if(id < 0) return -1;
    if(which == PN_CC){
        switch(id){
            case T_NET: { int a = app_by_icon(UI_ICO_NET); pn_close(); return a; }
            case T_DND: ui_prefs.dnd = !ui_prefs.dnd; ui_prefs_changed(); break;
            case T_ANIM: ui_anim_on = !ui_anim_on; gui_damage_all(); break;
            case T_DOCK: ui_prefs.dock_on = !ui_prefs.dock_on; ui_prefs_changed(); break;
            case T_WIDGETS: ui_prefs.wid_on = !ui_prefs.wid_on; ui_prefs_changed(); break;
            case T_ISLAND: ui_prefs.isl_idle = (ui_prefs.isl_idle + 1) % ISL_IDLE_N; ui_prefs_changed(); break;
            case S_VOL: case S_BRI: drag = id; set_slider(id, R, x); break;
            case WALL: ui_wall_set((ui_wall_index() + 1) % ui_wall_count()); break;
            default:
                if(id >= ACC0 && id < ACC0 + UI_NACCENT){ ui_set_accent(id - ACC0); ui_prefs_changed(); }
                else if(id >= APP0 && id < APP0 + NSHORT){
                    int a = short_app(id - APP0);
                    if(a >= 0){ pn_close(); return a; }
                }
        }
        return -1;
    }
    /* Notification Center */
    if(id == N_DND){ ui_prefs.dnd = !ui_prefs.dnd; ui_prefs_changed(); return -1; }
    if(id == N_CLEAR){ ui_hist_clear(); scroll = 0; return -1; }
    if(id >= N_X){ ui_hist_remove(scroll + id - N_X); if(scroll > nc_max_scroll()) scroll = nc_max_scroll(); return -1; }
    {
        int idx = scroll + id - N_ENTRY, app = -1;
        ui_hist_get(idx, &app, 0, 0, 0, 0, 0);
        ui_hist_remove(idx);
        if(scroll > nc_max_scroll()) scroll = nc_max_scroll();
        if(app >= 0){ pn_close(); return app; }
    }
    return -1;
}

void pn_scroll(int dz){
    if(which != PN_NC) return;
    scroll += dz > 0 ? 1 : -1;
    if(scroll < 0) scroll = 0;
    if(scroll > nc_max_scroll()) scroll = nc_max_scroll();
}

/* Every GUI turn: what the pointer is over, and a slider that is being dragged. */
void pn_update(rect_t bar, u32 sw, int mx, int my, int held){
    if(!which){ hover = -1; return; }
    rect_t R = pn_rect(bar, sw);
    hover = ui_contains(R, mx, my) ? item_at(R, mx, my) : -1;
    if(drag >= 0){
        if(held & 1) set_slider(drag, R, mx);
        else drag = -1;
    }
}

int pn_key(int c){
    if(!which) return 0;
    if(c == 27){ pn_close(); return 1; }
    return 0;
}

/* ---- repainting ---- */
static u32 hash_bytes(const void *p, u32 n){
    const u8 *b = p; u32 h = 2166136261u;
    for(u32 i = 0; i < n; i++){ h ^= b[i]; h *= 16777619u; }
    return h;
}

void pn_damage(rect_t bar, u32 sw, u64 now){
    static int sh_which = PN_NONE;
    static u32 sh_sig;
    static rect_t sh_rect;
    u32 sig = 0;
    rect_t R = pn_rect(bar, sw);
    if(which){
        int dy = slide_dy(now);
        char clk[8]; ui_clock(clk);
        u32 parts[10] = { hash_bytes(&ui_prefs, sizeof ui_prefs), (u32)hover, (u32)drag, (u32)ui_hist_count(),
                          (u32)scroll, (u32)ui_accent_index() | ((u32)ui_anim_on << 8) | ((u32)net_up() << 9),
                          (u32)ui_wall_index(), (u32)dy, hash_bytes(clk, 5), (u32)(now / 6000) };
        sig = hash_bytes(parts, sizeof parts);
        R.y += (u32)dy;
    }
    if(which != sh_which || sig != sh_sig){
        gui_damage((int)sh_rect.x - 6, (int)sh_rect.y - 4, (int)sh_rect.w + 16, (int)sh_rect.h + 20);
        if(which) gui_damage((int)R.x - 6, (int)R.y - 4, (int)R.w + 16, (int)R.h + 20);
        sh_which = which; sh_sig = sig; sh_rect = R;
    }
}

/* ---- drawing ---- */
static void glyph_for(int id, int cx, int cy, u32 col, u32 hole){
    switch(id){
        case T_NET:                                     /* signal bars */
            for(int i = 0; i < 4; i++) ui_rrect((u32)(cx - 12 + i * 7), (u32)(cy + 10 - (i + 1) * 5), 4, (u32)((i + 1) * 5), 1, col);
            break;
        case T_DND:                                     /* a crescent moon */
            ui_disc(cx, cy, 11, col);
            ui_disc(cx + 6, cy - 5, 9, hole);
            break;
        case T_ANIM:                                    /* a play triangle in a ring */
            ui_ring(cx, cy, 12, 2, col);
            ui_line(cx - 3, cy - 6, cx - 3, cy + 6, 3, col);
            ui_line(cx - 3, cy - 6, cx + 6, cy, 3, col);
            ui_line(cx - 3, cy + 6, cx + 6, cy, 3, col);
            break;
        case T_DOCK:                                    /* a bar with icons on it */
            ui_rrect((u32)(cx - 13), (u32)(cy + 2), 26, 10, 5, col);
            ui_rrect((u32)(cx - 8), (u32)(cy - 11), 7, 7, 2, col);
            ui_rrect((u32)(cx + 2), (u32)(cy - 11), 7, 7, 2, col);
            break;
        case T_WIDGETS:                                 /* a grid of cards */
            ui_rrect((u32)(cx - 12), (u32)(cy - 12), 14, 14, 3, col);
            ui_rrect((u32)(cx + 4), (u32)(cy - 12), 8, 14, 3, col);
            ui_rrect((u32)(cx - 12), (u32)(cy + 4), 8, 8, 3, col);
            ui_rrect((u32)(cx - 2), (u32)(cy + 4), 14, 8, 3, col);
            break;
        case T_ISLAND:                                  /* a pill */
            ui_rrect((u32)(cx - 14), (u32)(cy - 7), 28, 14, 7, col);
            ui_disc(cx + 7, cy, 3, hole);
            break;
    }
}

static const char *idle_name(int m){
    static const char *n[ISL_IDLE_N] = { "Name", "Clock", "CPU & RAM", "Hidden" };
    return n[m];
}

static void draw_tile(int id, rect_t R, int on, const char *label, const char *sub){
    rect_t t = cc_item(id, R);
    u32 base = on ? ui_accent() : C_CARD;
    u32 col = hover == id ? (on ? ui_lerp(ui_accent(), 0xFFFFFF, 40) : C_CARD_HI) : base;
    ui_rrect(t.x, t.y, t.w, t.h, 18, col);
    u32 ink = on ? 0x0B0F12 : C_TEXT;
    u32 disc = on ? 0x0B0F12 : C_CARD_HI;
    int cx = (int)t.x + 36, cy = (int)t.y + (int)t.h / 2;
    ui_disc(cx, cy, 22, disc);
    glyph_for(id, cx, cy, on ? ui_accent() : C_TEXT, disc);
    ui_text(t.x + 68, t.y + 15, label, ink, 1);
    ui_text(t.x + 68, t.y + 37, sub, on ? 0x1E2A33 : C_MUTED, 1);
}

static void draw_slider(int id, rect_t R, const char *label, int pct, int right_val){
    rect_t s = cc_item(id, R);
    char v[12], txt[16];
    ui_num((u32)right_val, v); ui_copy(txt, v, sizeof txt); ui_cat(txt, "%");
    ui_text(s.x, s.y - 22, label, C_MUTED, 1);
    ui_text(s.x + s.w - ui_text_w(txt, 1), s.y - 22, txt, C_TEXT, 1);
    int lo = (int)s.x + 12, hi = (int)(s.x + s.w) - 12, cy = (int)s.y + 14;
    int kx = lo + (hi - lo) * pct / 100;
    ui_rrect((u32)lo - 8, (u32)cy - 6, (u32)(hi - lo) + 16, 12, 6, C_CARD_HI);
    ui_rrect((u32)lo - 8, (u32)cy - 6, (u32)(kx - lo) + 8, 12, 6, ui_accent());
    ui_disc(kx, cy, (hover == id || drag == id) ? 13 : 11, C_TEXT);
}

static void draw_cc(rect_t R){
    ui_text(R.x + 20, R.y + 20, "Control Center", C_TEXT, UI_SCALE);
    char sub[32];
    if(net_up()){ u32 ip = 0, gw = 0; u8 mac[6]; net_info(&ip, &gw, mac); ui_ip(sub, ip); }
    else ui_copy(sub, "Offline", sizeof sub);
    draw_tile(T_NET, R, net_up(), "Network", sub);
    draw_tile(T_DND, R, ui_prefs.dnd, "Do Not Disturb", ui_prefs.dnd ? "On" : "Off");
    draw_tile(T_ANIM, R, ui_anim_on, "Animations", ui_anim_on ? "On" : "Off");
    draw_tile(T_DOCK, R, ui_prefs.dock_on, "Dock", ui_prefs.dock_on ? "On" : "Off");
    draw_tile(T_WIDGETS, R, ui_prefs.wid_on, "Widgets", ui_prefs.wid_on ? "On" : "Off");
    draw_tile(T_ISLAND, R, ui_prefs.isl_idle != ISL_IDLE_HIDDEN, "Island", idle_name(ui_prefs.isl_idle));

    draw_slider(S_VOL, R, "Volume", ui_prefs.volume, ui_prefs.volume);
    draw_slider(S_BRI, R, "Brightness", (ui_prefs.brightness - 20) * 100 / 80, ui_prefs.brightness);

    ui_text(R.x + 20, R.y + 426, "Accent", C_MUTED, 1);
    for(int i = 0; i < UI_NACCENT; i++){
        rect_t d = cc_item(ACC0 + i, R);
        if(ui_accent_index() == i) ui_ring((int)d.x + 18, (int)d.y + 18, 18, 3, C_TEXT);
        else if(hover == ACC0 + i) ui_ring((int)d.x + 18, (int)d.y + 18, 18, 2, C_DIM);
        ui_disc((int)d.x + 18, (int)d.y + 18, 13, ui_accent_color_of(i));
    }
    rect_t w = cc_item(WALL, R);
    ui_rrect(w.x, w.y, w.w, w.h, 14, hover == WALL ? C_CARD_HI : C_CARD);
    const char *wl = "Next wallpaper";
    ui_text(w.x + (w.w - ui_text_w(wl, 1)) / 2, w.y + (w.h - 16) / 2, wl, C_TEXT, 1);

    for(int i = 0; i < NSHORT; i++){
        rect_t b = cc_item(APP0 + i, R);
        ui_rrect(b.x, b.y, b.w, b.h, 16, hover == APP0 + i ? C_CARD_HI : C_CARD);
        ui_icon(SHORT_ICONS[i], b.x + 12, b.y + 12, hover == APP0 + i ? C_TEXT : C_MUTED);
    }
    if(hover >= APP0 && hover < APP0 + NSHORT){
        int a = short_app(hover - APP0);
        const char *nm = a == UI_MAX_APPS ? "Terminal" : a >= 0 ? ui_apps[a]->name : "";
        ui_text(R.x + (R.w - ui_text_w(nm, 1)) / 2, R.y + 574, nm, C_MUTED, 1);
    }
}

static void draw_nc(rect_t R, u64 now){
    char clk[8], date[32];
    ui_clock(clk);
    ui_date_text(date, sizeof date);
    ui_text(R.x + 20, R.y + 20, clk, C_TEXT, 4);
    ui_text(R.x + 22, R.y + 84, date, C_MUTED, UI_SCALE);

    rect_t d = nc_item(N_DND, R);
    ui_rrect(d.x, d.y, d.w, d.h, 18, ui_prefs.dnd ? ui_accent() : (hover == N_DND ? C_CARD_HI : C_CARD));
    glyph_for(T_DND, (int)d.x + 20, (int)d.y + 18, ui_prefs.dnd ? 0x0B0F12 : C_TEXT, ui_prefs.dnd ? ui_accent() : (hover == N_DND ? C_CARD_HI : C_CARD));
    ui_text(d.x + 42, d.y + 10, ui_prefs.dnd ? "Do Not Disturb: on" : "Do Not Disturb", ui_prefs.dnd ? 0x0B0F12 : C_TEXT, 1);
    int n = ui_hist_count();
    if(n){
        rect_t c = nc_item(N_CLEAR, R);
        ui_rrect(c.x, c.y, c.w, c.h, 18, hover == N_CLEAR ? C_CARD_HI : C_CARD);
        ui_text(c.x + (c.w - ui_text_w("Clear all", 1)) / 2, c.y + 10, "Clear all", C_TEXT, 1);
    }
    ui_fill((rect_t){ R.x + 20, R.y + NC_TOP - 8, R.w - 40, 1 }, C_SEP);
    if(!n){
        const char *a = "No notifications", *b = "You are all caught up";
        ui_text(R.x + (R.w - ui_text_w(a, UI_SCALE)) / 2, R.y + 300, a, C_MUTED, UI_SCALE);
        ui_text(R.x + (R.w - ui_text_w(b, 1)) / 2, R.y + 340, b, C_DIM, 1);
        return;
    }
    int vis = nc_visible();
    for(int i = 0; i < vis && scroll + i < n; i++){
        int app; char title[40], body[72]; u64 at;
        ui_hist_get(scroll + i, &app, title, sizeof title, body, sizeof body, &at);
        rect_t e = nc_item(N_ENTRY + i, R);
        int hot = hover == N_ENTRY + i || hover == N_X + i;
        ui_rrect(e.x, e.y, e.w, e.h, 16, hot ? C_CARD_HI : C_CARD);
        /* the icon */
        ui_rrect(e.x + 10, e.y + 10, 44, 44, 22, ui_accent());
        if(app >= 0 && app < ui_app_count()) ui_icon(ui_apps[app]->icon, e.x + 16, e.y + 16, C_BAR);
        else {
            ui_fill((rect_t){ e.x + 28, e.y + 20, 8, 3 }, C_BAR);
            ui_fill((rect_t){ e.x + 25, e.y + 23, 14, 12 }, C_BAR);
            ui_fill((rect_t){ e.x + 22, e.y + 35, 20, 3 }, C_BAR);
        }
        char t[48];
        ui_copy(t, title, sizeof t);
        u32 maxt = (e.w - 70 - 12) / 16;
        if(ui_len(t) > maxt){ t[maxt - 2] = '.'; t[maxt - 1] = '.'; t[maxt] = 0; }
        ui_text(e.x + 66, e.y + 6, t, C_TEXT, UI_SCALE);
        char age[8];
        age_text(age, at, now);
        char bd[72];
        ui_copy(bd, body, sizeof bd);
        u32 maxb = (e.w - 70 - 20 - ui_text_w(age, 1)) / 8;
        if(ui_len(bd) > maxb){ bd[maxb - 2] = '.'; bd[maxb - 1] = '.'; bd[maxb] = 0; }
        ui_text(e.x + 66, e.y + 42, bd, C_MUTED, 1);
        ui_text(e.x + e.w - 12 - ui_text_w(age, 1), e.y + 42, age, C_DIM, 1);
        if(hot){                                       /* the dismiss cross */
            rect_t x = nc_item(N_X + i, R);
            ui_disc((int)x.x + 16, (int)x.y + 16, 12, hover == N_X + i ? C_HOVER : C_CARD);
            ui_line((int)x.x + 11, (int)x.y + 11, (int)x.x + 21, (int)x.y + 21, 2, C_TEXT);
            ui_line((int)x.x + 21, (int)x.y + 11, (int)x.x + 11, (int)x.y + 21, 2, C_TEXT);
        }
    }
    if(n > vis){                                       /* a scroll bar */
        u32 th = (u32)((PN_H - NC_TOP - 12) * vis / n);
        u32 ty = (u32)((PN_H - NC_TOP - 12 - (int)th) * scroll / (nc_max_scroll() ? nc_max_scroll() : 1));
        ui_rrect(R.x + R.w - 8, R.y + NC_TOP + ty, 4, th, 2, C_CARD_HI);
    }
}

void pn_draw(rect_t bar, u32 sw, u64 now){
    if(!which) return;
    rect_t R = pn_rect(bar, sw);
    R.y += (u32)slide_dy(now);
    if(!fb_clip_hits((i32)R.x - 6, (i32)R.y - 4, (i32)R.w + 16, (i32)R.h + 20)) return;
    ui_rrect(R.x + 3, R.y + 8, R.w, R.h, 26, C_SHADOW);
    ui_panel(R.x, R.y, R.w, R.h, 26, C_BAR_EDGE, C_MENU);
    if(which == PN_CC) draw_cc(R);
    else draw_nc(R, now);
}
