/* Settings: a list of pages on the left, the page's controls on the right (scrolls when the
 * window is short). Every control is described once, in page_body(), which is run three
 * ways: to draw, to handle a click and to follow a slider that is being dragged - so what
 * you see is always what you hit. */
#include "amdroid/ui.h"
#include "amdroid/net.h"
#include "amdroid/hda.h"
#include "amdroid/fb.h"
#include "amdroid/keyboard.h"

extern int browser_adblock, browser_sort_downloads;     /* apps/browser.c */

enum { P_APPEARANCE, P_ISLAND, P_DOCK, P_WIDGETS, P_SOUND, P_BROWSER, P_NETWORK, P_N };
static const char *PAGE_NAME[P_N] = { "Appearance", "Dynamic Island", "Dock", "Widgets", "Sound & screen", "Browser", "Network" };

static int page = P_APPEARANCE;
static int scroll = 0, content_h = 0;
static int drag_id = -1;                     /* the slider being dragged */
static int next_id = 0;

enum { M_DRAW, M_CLICK, M_DRAG };
typedef struct {
    rect_t pane;
    int y;                                   /* the next control's top, from the top of the content */
    int mode, cx, cy, hit;
} ctx_t;

void ui_settings_page(int p){
    if(p >= 0 && p < P_N){ page = p; scroll = 0; }
}

static int side_w(rect_t b){ return b.w < 560 ? 150 : 190; }

/* ---- the pieces ---- */
static rect_t take(ctx_t *c, int h){
    rect_t r = { c->pane.x, (u32)((int)c->pane.y + c->y - scroll), c->pane.w, (u32)h };
    c->y += h;
    return r;
}
static int seen(ctx_t *c, rect_t r){ return (int)(r.y + r.h) > (int)c->pane.y && (int)r.y < (int)(c->pane.y + c->pane.h); }
/* This control was clicked (the click is used up). */
static int pressed(ctx_t *c, rect_t r){
    if(c->mode != M_CLICK || c->hit) return 0;
    if(!ui_contains(c->pane, c->cx, c->cy) || !ui_contains(r, c->cx, c->cy)) return 0;
    c->hit = 1;
    return 1;
}

static void heading(ctx_t *c, const char *t){
    if(c->y > 0) c->y += 14;
    rect_t r = take(c, 30);
    if(c->mode == M_DRAW && seen(c, r)) ui_text(r.x, r.y + 8, t, ui_accent(), 1);
}
static void note(ctx_t *c, const char *t){
    rect_t r = take(c, 28);
    if(c->mode == M_DRAW && seen(c, r)) ui_text(r.x, r.y + 6, t, C_MUTED, 1);
}
static void gap(ctx_t *c, int h){ c->y += h; }

/* a switch with its label; 1 when it was clicked */
static int toggle(ctx_t *c, const char *label, int on){
    rect_t r = take(c, 44);
    if(c->mode == M_DRAW && seen(c, r)){
        ui_text(r.x, r.y + 14, label, on ? C_TEXT : C_MUTED, 1);
        u32 sx = r.x + r.w - 64;
        ui_rrect(sx, r.y + 6, 64, 32, 16, on ? ui_accent() : C_CARD_HI);
        ui_rrect(on ? sx + 36 : sx + 4, r.y + 10, 24, 24, 12, C_TEXT);
    }
    return pressed(c, r);
}

/* a row of choices; returns the chosen index when one was clicked, else -1 */
static int segmented(ctx_t *c, const char *label, const char *const *opts, int n, int cur){
    if(label){ rect_t l = take(c, 22); if(c->mode == M_DRAW && seen(c, l)) ui_text(l.x, l.y + 2, label, C_MUTED, 1); }
    rect_t r = take(c, 40);
    gap(c, 10);
    int res = -1;
    u32 w = r.w / (u32)n;
    for(int i = 0; i < n; i++){
        rect_t o = { r.x + (u32)i * w, r.y, i == n - 1 ? r.w - (u32)i * w : w - 4, r.h };
        if(c->mode == M_DRAW && seen(c, r)){
            ui_rrect(o.x, o.y, o.w, o.h, 12, i == cur ? ui_accent() : C_CARD);
            u32 tw = ui_text_w(opts[i], 1);
            ui_text(o.x + (o.w - tw) / 2, o.y + 12, opts[i], i == cur ? 0x0B0F12 : C_TEXT, 1);
        }
        if(pressed(c, o)) res = i;
    }
    return res;
}

/* a slider from lo to hi; returns the new value while it is clicked or dragged, else -1 */
static int slider(ctx_t *c, const char *label, int val, int lo, int hi, const char *unit){
    int id = next_id++;
    rect_t l = take(c, 22);
    char v[16], txt[24];
    ui_num((u32)val, v); ui_copy(txt, v, sizeof txt); ui_cat(txt, unit);
    if(c->mode == M_DRAW && seen(c, l)){
        ui_text(l.x, l.y + 2, label, C_MUTED, 1);
        ui_text(l.x + l.w - ui_text_w(txt, 1), l.y + 2, txt, C_TEXT, 1);
    }
    rect_t r = take(c, 32);
    gap(c, 12);
    int a = (int)r.x + 12, b = (int)(r.x + r.w) - 12;
    if(c->mode == M_DRAW && seen(c, r)){
        int cy = (int)r.y + 16, kx = a + (b - a) * (val - lo) / (hi - lo);
        ui_rrect((u32)a - 8, (u32)cy - 6, (u32)(b - a) + 16, 12, 6, C_CARD_HI);
        ui_rrect((u32)a - 8, (u32)cy - 6, (u32)(kx - a) + 8, 12, 6, ui_accent());
        ui_disc(kx, cy, drag_id == id ? 14 : 12, C_TEXT);
    }
    int px = -1;
    if(c->mode == M_CLICK && pressed(c, r)){ drag_id = id; px = c->cx; }
    else if(c->mode == M_DRAG && drag_id == id) px = c->cx;
    if(px < 0) return -1;
    int v2 = lo + (px - a) * (hi - lo) / (b - a);
    return v2 < lo ? lo : v2 > hi ? hi : v2;
}

static int button(ctx_t *c, const char *label, u32 w){
    rect_t r = take(c, 40);
    gap(c, 10);
    rect_t b = { r.x, r.y, w, r.h };
    if(c->mode == M_DRAW && seen(c, r)){
        ui_rrect(b.x, b.y, b.w, b.h, 12, C_CARD_HI);
        ui_text(b.x + (b.w - ui_text_w(label, 1)) / 2, b.y + 12, label, C_TEXT, 1);
    }
    return pressed(c, b);
}

static void changed(void){ ui_prefs_changed(); }

/* ---- the pages ---- */
static void page_appearance(ctx_t *c){
    heading(c, "ACCENT COLOR");
    rect_t r = take(c, 104);
    for(int i = 0; i < UI_NACCENT; i++){
        rect_t s = { r.x + (u32)i * 92, r.y + 6, 64, 64 };
        if(c->mode == M_DRAW && seen(c, r)){
            if(ui_accent_index() == i) ui_rrect(s.x - 5, s.y - 5, s.w + 10, s.h + 10, 14, C_TEXT);
            ui_rrect(s.x, s.y, s.w, s.h, 10, ui_accent_color_of(i));
            u32 nw = ui_text_w(ui_accent_name(i), 1);
            ui_text(s.x + (s.w - nw) / 2, s.y + s.h + 10, ui_accent_name(i), ui_accent_index() == i ? C_TEXT : C_MUTED, 1);
        }
        if(pressed(c, s)){ ui_set_accent(i); changed(); }
    }
    heading(c, "WALLPAPER");
    int cols = (int)(c->pane.w + 24) / 172;
    if(cols < 1) cols = 1;
    int count = ui_wall_count(), rows = (count + cols - 1) / cols;
    rect_t w = take(c, rows * 124);
    for(int i = 0; i < count; i++){
        rect_t t = { w.x + (u32)(i % cols) * 172, w.y + 6 + (u32)(i / cols) * 124, 148, 76 };
        if(c->mode == M_DRAW && seen(c, w)){
            if(ui_wall_index() == i) ui_rrect(t.x - 5, t.y - 5, t.w + 10, t.h + 10, 12, C_TEXT);
            ui_rrect(t.x, t.y, t.w, t.h, 8, ui_wall_swatch(i));
            ui_rrect(t.x + t.w / 2, t.y + t.h - 26, t.w / 2 - 6, 4, 2, 0x8A96A0);     /* a hint of the light swoosh */
            ui_rrect(t.x + t.w / 2 + 10, t.y + t.h - 16, t.w / 2 - 16, 3, 1, 0x6A7680);
            u32 nw = ui_text_w(ui_wall_name(i), 1);
            ui_text(t.x + (t.w - nw) / 2, t.y + t.h + 10, ui_wall_name(i), ui_wall_index() == i ? C_TEXT : C_MUTED, 1);
        }
        if(pressed(c, t)) ui_wall_set(i);
    }
    heading(c, "GLASS");
    if(toggle(c, ui_prefs.glass ? "Glass: on" : "Glass: off", ui_prefs.glass)){ ui_prefs.glass = !ui_prefs.glass; changed(); }
    if(ui_prefs.glass){
        static const char *blur[] = { "Soft", "Medium", "Strong" };
        static const char *tint[] = { "Clear", "Frosted", "Dark" };
        int r;
        if((r = segmented(c, "Blur", blur, 3, ui_prefs.glass_blur)) >= 0){ ui_prefs.glass_blur = r; changed(); }
        if((r = segmented(c, "Tint", tint, 3, ui_prefs.glass_tint)) >= 0){ ui_prefs.glass_tint = r; changed(); }
    }
    note(c, "Windows, the bar, menus and panels become frosted glass.");
    heading(c, "MOTION");
    if(toggle(c, ui_anim_on ? "Animations: on" : "Animations: off", ui_anim_on)){ ui_anim_on = !ui_anim_on; changed(); }
}

static void page_island(ctx_t *c){
    static const char *idle[] = { "Name", "Clock", "CPU & RAM", "Hidden" };
    static const char *size[] = { "Small", "Medium", "Large" };
    static const char *style[] = { "Black", "Accent", "Outline" };
    static const char *secs[] = { "2 s", "4 s", "8 s", "12 s" };
    static const int secv[] = { 2, 4, 8, 12 };
    int r;
    heading(c, "LOOK");
    if((r = segmented(c, "Resting face", idle, 4, ui_prefs.isl_idle)) >= 0){ ui_prefs.isl_idle = r; changed(); }
    if((r = segmented(c, "Size", size, 3, ui_prefs.isl_size)) >= 0){ ui_prefs.isl_size = r; changed(); }
    if((r = segmented(c, "Style", style, 3, ui_prefs.isl_style)) >= 0){ ui_prefs.isl_style = r; changed(); }
    heading(c, "NOTIFICATIONS");
    int cur = 1;
    for(int i = 0; i < 4; i++) if(secv[i] == ui_prefs.isl_secs) cur = i;
    if((r = segmented(c, "A notification stays for", secs, 4, cur)) >= 0){ ui_prefs.isl_secs = secv[r]; changed(); }
    if(toggle(c, ui_prefs.dnd ? "Do Not Disturb: on" : "Do Not Disturb: off", ui_prefs.dnd)){ ui_prefs.dnd = !ui_prefs.dnd; changed(); }
    if(button(c, "Send a test notification", 240)){
        int set = -1;
        for(int a = 0; a < ui_app_count(); a++) if(ui_apps[a]->icon == UI_ICO_SETTINGS) set = a;
        ui_notify(set, "Test notification", "This is how the island looks now");
    }
    heading(c, "POSITION");
    note(c, "Drag the island anywhere on the desktop.");
    if(toggle(c, ui_prefs.isl_snap ? "Snap to edges: on" : "Snap to edges: off", ui_prefs.isl_snap)){ ui_prefs.isl_snap = !ui_prefs.isl_snap; changed(); }
    if(button(c, "Back to the top centre", 240)){ ui_prefs.isl_x = ui_prefs.isl_y = -1; changed(); }
}

static void page_dock(ctx_t *c){
    static const char *edge[] = { "Right", "Left", "Bottom" };
    static const char *size[] = { "Small", "Medium", "Large" };
    static const char *zoom[] = { "Off", "Low", "High" };
    int r;
    heading(c, "DOCK");
    if(toggle(c, ui_prefs.dock_on ? "Show the dock: on" : "Show the dock: off", ui_prefs.dock_on)){ ui_prefs.dock_on = !ui_prefs.dock_on; changed(); }
    if((r = segmented(c, "Edge of the screen", edge, 3, ui_prefs.dock_edge)) >= 0){ ui_prefs.dock_edge = r; changed(); }
    if((r = segmented(c, "Icon size", size, 3, ui_prefs.dock_size)) >= 0){ ui_prefs.dock_size = r; changed(); }
    if((r = segmented(c, "Magnification under the pointer", zoom, 3, ui_prefs.dock_zoom)) >= 0){ ui_prefs.dock_zoom = r; changed(); }
    note(c, "Right-click a dock icon for more.");
}

static void page_widgets(ctx_t *c){
    static const char *names[UI_NWIDGETS] = { "Clock", "System", "Calendar", "Network", "Notes" };
    heading(c, "WIDGETS");
    if(toggle(c, ui_prefs.wid_on ? "Show widgets: on" : "Show widgets: off", ui_prefs.wid_on)){ ui_prefs.wid_on = !ui_prefs.wid_on; changed(); }
    gap(c, 6);
    for(int i = 0; i < UI_NWIDGETS; i++){
        int on = (ui_prefs.wid_mask >> i) & 1;
        if(toggle(c, names[i], on)){ ui_prefs.wid_mask ^= 1 << i; changed(); }
    }
    note(c, "Drag a widget to move it. Right-click it to remove it.");
    if(button(c, "Reset places", 200)){
        for(int i = 0; i < UI_NWIDGETS; i++) ui_prefs.wid_x[i] = ui_prefs.wid_y[i] = -1;
        changed();
    }
}

static void page_sound(ctx_t *c){
    int v;
    heading(c, "SOUND");
    if((v = slider(c, "Volume", ui_prefs.volume, 0, 100, "%")) >= 0){ ui_prefs.volume = v; ui_prefs_live(); }
    if(hda_present()){
        if(button(c, "Play a test sound", 220)) hda_play_tone(660, 250);
    } else note(c, "No sound card found.");
    heading(c, "SCREEN");
    if((v = slider(c, "Brightness", ui_prefs.brightness, 20, 100, "%")) >= 0){ ui_prefs.brightness = v; ui_prefs_live(); }
}

static void page_browser(ctx_t *c){
    heading(c, "BROWSER");
    if(toggle(c, browser_adblock ? "Block ads: on" : "Block ads: off", browser_adblock)) browser_adblock = !browser_adblock;
    if(toggle(c, browser_sort_downloads ? "Sort downloads: on" : "Sort downloads: off", browser_sort_downloads)) browser_sort_downloads = !browser_sort_downloads;
    note(c, "Sorted downloads go into folders by type.");
}

static void page_network(ctx_t *c){
    heading(c, "NETWORK");
    rect_t r = take(c, 120);
    if(c->mode != M_DRAW || !seen(c, r)) return;
    if(net_up()){
        u32 ip = 0, gw = 0; u8 mac[6];
        net_info(&ip, &gw, mac);
        char s[40], l[64];
        ui_disc((int)r.x + 8, (int)r.y + 14, 6, C_GOOD);
        ui_text(r.x + 26, r.y + 4, "Connected", C_TEXT, UI_SCALE);
        ui_ip(s, ip); ui_copy(l, "Address  ", sizeof l); ui_cat(l, s);
        ui_text(r.x, r.y + 44, l, C_MUTED, 1);
        ui_ip(s, gw); ui_copy(l, "Gateway  ", sizeof l); ui_cat(l, s);
        ui_text(r.x, r.y + 68, l, C_MUTED, 1);
        static const char hx[] = "0123456789abcdef";
        char m[24]; int k = 0;
        for(int i = 0; i < 6; i++){ m[k++] = hx[mac[i] >> 4]; m[k++] = hx[mac[i] & 15]; if(i < 5) m[k++] = ':'; }
        m[k] = 0;
        ui_copy(l, "MAC      ", sizeof l); ui_cat(l, m);
        ui_text(r.x, r.y + 92, l, C_MUTED, 1);
    } else {
        ui_disc((int)r.x + 8, (int)r.y + 14, 6, C_WARN);
        ui_text(r.x + 26, r.y + 4, "Not connected", C_TEXT, UI_SCALE);
    }
}

static void page_body(ctx_t *c){
    next_id = 0;
    c->y = 0;
    switch(page){
        case P_APPEARANCE: page_appearance(c); break;
        case P_ISLAND: page_island(c); break;
        case P_DOCK: page_dock(c); break;
        case P_WIDGETS: page_widgets(c); break;
        case P_SOUND: page_sound(c); break;
        case P_BROWSER: page_browser(c); break;
        case P_NETWORK: page_network(c); break;
    }
}

static rect_t pane_of(rect_t b){
    u32 sw = (u32)side_w(b);
    return (rect_t){ b.x + sw + 20, b.y, b.w - sw - 20 - 10, b.h };
}
static rect_t page_rect(rect_t b, int i){ return (rect_t){ b.x, b.y + (u32)i * 46, (u32)side_w(b) - 6, 42 }; }

static void clamp_scroll(rect_t pane){
    int max = content_h - (int)pane.h + 8;
    if(max < 0) max = 0;
    if(scroll > max) scroll = max;
    if(scroll < 0) scroll = 0;
}

static void s_draw(rect_t b, int focused){
    (void)focused;
    /* the page list */
    for(int i = 0; i < P_N; i++){
        rect_t r = page_rect(b, i);
        if(i == page){
            ui_rrect(r.x, r.y, r.w, r.h, 12, C_CARD_HI);
            ui_rrect(r.x + 6, r.y + 11, 4, 20, 2, ui_accent());
        }
        ui_text(r.x + 20, r.y + 13, PAGE_NAME[i], i == page ? C_TEXT : C_MUTED, 1);
    }
    ui_fill((rect_t){ b.x + (u32)side_w(b) + 6, b.y, 1, b.h }, C_SEP);

    ctx_t c = { .pane = pane_of(b), .mode = M_DRAW };
    clamp_scroll(c.pane);
    fb_set_clip(c.pane.x - 4, c.pane.y, c.pane.w + 14, c.pane.h);       /* the page scrolls inside its pane */
    page_body(&c);
    content_h = c.y;
    clamp_scroll(c.pane);
    if(content_h > (int)c.pane.h){                                       /* the scroll bar */
        u32 th = (u32)((int)c.pane.h * (int)c.pane.h / content_h);
        if(th < 24) th = 24;
        int max = content_h - (int)c.pane.h + 8;
        u32 ty = max > 0 ? (u32)(((int)c.pane.h - (int)th) * scroll / max) : 0;
        ui_rrect(c.pane.x + c.pane.w + 2, c.pane.y + ty, 4, th, 2, C_CARD_HI);
    }
    fb_set_clip(b.x, b.y, b.w, b.h);
}

static void s_click(rect_t b, int x, int y){
    for(int i = 0; i < P_N; i++)
        if(ui_contains(page_rect(b, i), x, y)){ if(page != i){ page = i; scroll = 0; } return; }
    ctx_t c = { .pane = pane_of(b), .mode = M_CLICK, .cx = x, .cy = y };
    page_body(&c);
}

/* The pointer moved over the window: a slider that is held follows it. */
static void s_move(rect_t b, int x, int y){
    if(drag_id < 0) return;
    if(!(ui_mouse_held() & 1) || x < 0){ drag_id = -1; ui_redraw(); return; }
    ctx_t c = { .pane = pane_of(b), .mode = M_DRAG, .cx = x, .cy = y };
    page_body(&c);
    ui_redraw();
}

static void s_scroll(rect_t b, int x, int y, int dz){
    (void)x; (void)y;
    scroll += dz * 48;
    clamp_scroll(pane_of(b));
}

static int s_key(int c){
    if(c == KEY_DOWN){ if(page < P_N - 1){ page++; scroll = 0; } return 1; }
    if(c == KEY_UP){ if(page > 0){ page--; scroll = 0; } return 1; }
    if(c == KEY_PGDN){ scroll += 240; return 1; }
    if(c == KEY_PGUP){ scroll -= 240; if(scroll < 0) scroll = 0; return 1; }
    if(c == 'a' || c == 'A'){ ui_anim_on = !ui_anim_on; ui_prefs_changed(); return 1; }
    if(c == 'w' || c == 'W'){ ui_wall_set((ui_wall_index() + 1) % ui_wall_count()); return 1; }
    return 0;
}

const app_t app_settings = {
    .name = "Settings",
    .icon = UI_ICO_SETTINGS,
    .open = NULL,
    .close = NULL,
    .draw = s_draw,
    .key = s_key,
    .click = s_click,
    .move = s_move,
    .scroll = s_scroll,
};
