/* The dock: every app as a tile along a screen edge (right, left or above the taskbar).
 * Move the pointer over it and the tiles under and near it grow, smoothly, like a lens; the
 * dock stretches with them and keeps the tile under the pointer where it is. A dot marks an
 * app that is open. Click to open, raise or minimize. */
#include "gui_internal.h"
#include "amdroid/fb.h"

#define PAD 8                               /* gap between tiles, and around them */
#define EDGE_GAP 10                         /* between the dock and the screen edge */
#define BOTTOM_RESERVE 80                   /* the taskbar and its margin */
#define FADE_STEP 24                        /* lens strength gained or lost per tick */

static int scr_w, scr_h;
static int ptr_along = 0;                   /* where the pointer is along the dock (kept while the lens fades) */
static int mag = 0;                         /* 0..255: how strongly the lens is on */
static int inside = 0;
static u64 last_now = 0;
static int hover_item = -1;

typedef struct { int n; rect_t t[UI_MAX_APPS]; rect_t pill; } layout_t;

void dk_init(u32 sw, u32 sh){ scr_w = (int)sw; scr_h = (int)sh; }

static int base_size(void){ static const int b[3] = { 40, 48, 56 }; return b[ui_prefs.dock_size]; }
static int max_size(int b){ return ui_prefs.dock_zoom == 1 ? b * 145 / 100 : ui_prefs.dock_zoom == 2 ? b * 190 / 100 : b; }
static int vertical(void){ return ui_prefs.dock_edge != DOCK_BOTTOM; }
static int absi(int v){ return v < 0 ? -v : v; }

/* The lens: 256 at the pointer, falling smoothly to 0 two and a half tiles away. */
static int lens(int dist, int slot){
    int reach = slot * 5 / 2;
    if(dist >= reach) return 0;
    int t = dist * 256 / reach;                          /* 0..256 */
    int u = 256 - t * t / 256;                           /* 1 - t^2 */
    return u * u / 256;                                  /* squared: a soft bell */
}

/* Where everything is, for the pointer and lens strength as they are now. */
static void layout(layout_t *L){
    int n = ui_app_count();
    L->n = n;
    int b = base_size(), bm = max_size(b), slot = b + PAD, vert = vertical();
    int mid = vert ? scr_h / 2 - 24 : scr_w / 2;
    int pill0 = mid - (PAD + n * slot) / 2;               /* the dock's start with no lens */
    int s[UI_MAX_APPS], cm[UI_MAX_APPS];
    int total = PAD;
    for(int i = 0; i < n; i++){
        int c = pill0 + PAD + i * slot + b / 2;
        int f = lens(absi(ptr_along - c), slot);
        s[i] = b + (bm - b) * f / 256 * mag / 255;
        total += s[i] + PAD;
    }
    int start = mid - total / 2;
    int p = start + PAD;
    for(int i = 0; i < n; i++){ cm[i] = p + s[i] / 2; p += s[i] + PAD; }
    /* keep what is under the pointer where it is: shift by how far the tiles next to the
     * pointer moved, blended so nothing jumps when the pointer passes from one to the next */
    if(mag > 0 && n > 0){
        int fi = (ptr_along - (pill0 + PAD + b / 2)) * 256 / slot;
        if(fi < 0) fi = 0;
        if(fi > (n - 1) * 256) fi = (n - 1) * 256;
        int i0 = fi >> 8, t = fi & 255, i1 = i0 + 1 < n ? i0 + 1 : i0;
        int sh0 = (pill0 + PAD + i0 * slot + b / 2) - cm[i0];
        int sh1 = (pill0 + PAD + i1 * slot + b / 2) - cm[i1];
        start += sh0 + (sh1 - sh0) * t / 256;
    }
    int thick = b + 2 * PAD;
    if(vert){
        int x = ui_prefs.dock_edge == DOCK_RIGHT ? scr_w - EDGE_GAP - thick : EDGE_GAP;
        L->pill = (rect_t){ (u32)x, (u32)start, (u32)thick, (u32)total };
    } else {
        L->pill = (rect_t){ (u32)start, (u32)(scr_h - BOTTOM_RESERVE - thick), (u32)total, (u32)thick };
    }
    int pos = start + PAD;
    for(int i = 0; i < n; i++){
        int sz = s[i];
        if(!vert) L->t[i] = (rect_t){ (u32)pos, (u32)((int)L->pill.y + thick - PAD - sz), (u32)sz, (u32)sz };
        else if(ui_prefs.dock_edge == DOCK_RIGHT) L->t[i] = (rect_t){ (u32)((int)L->pill.x + thick - PAD - sz), (u32)pos, (u32)sz, (u32)sz };
        else L->t[i] = (rect_t){ L->pill.x + PAD, (u32)pos, (u32)sz, (u32)sz };
        pos += sz + PAD;
    }
}

static int tile_at(const layout_t *L, int x, int y){
    for(int i = 0; i < L->n; i++) if(ui_contains(L->t[i], x, y)) return i;
    return -1;
}

/* How much of the right edge the dock takes (widgets keep clear of it). */
int dk_reserve_right(void){
    if(!ui_prefs.dock_on || ui_prefs.dock_edge != DOCK_RIGHT) return 0;
    return base_size() + 2 * PAD + EDGE_GAP + 8;
}

/* The same for the left edge (the desktop icons move over), and for the strip above the
 * taskbar when the dock lies along the bottom. */
int dk_reserve_left(void){
    if(!ui_prefs.dock_on || ui_prefs.dock_edge != DOCK_LEFT) return 0;
    return base_size() + 2 * PAD + EDGE_GAP + 8;
}
int dk_reserve_bottom(void){
    if(!ui_prefs.dock_on || ui_prefs.dock_edge != DOCK_BOTTOM) return 0;
    return base_size() + 2 * PAD + 10;
}

int dk_over(int x, int y){
    if(!ui_prefs.dock_on) return 0;
    layout_t L; layout(&L);
    return ui_contains(L.pill, x, y) || tile_at(&L, x, y) >= 0;
}

int dk_hit(int x, int y){
    if(!ui_prefs.dock_on) return -1;
    layout_t L; layout(&L);
    return tile_at(&L, x, y);
}

void dk_update(int mx, int my, int blocked, u64 now){
    u64 steps = now - last_now > 8 ? 8 : now - last_now;
    last_now = now;
    if(!ui_prefs.dock_on){ mag = 0; inside = 0; hover_item = -1; return; }
    layout_t L; layout(&L);
    inside = !blocked && (ui_contains(L.pill, mx, my) || tile_at(&L, mx, my) >= 0);
    if(inside) ptr_along = vertical() ? my : mx;
    int target = inside && ui_prefs.dock_zoom ? 255 : 0;
    if(!ui_anim_on) mag = target;
    else for(u64 k = 0; k < steps; k++){
        if(mag < target) mag = mag + FADE_STEP > target ? target : mag + FADE_STEP;
        else if(mag > target) mag = mag - FADE_STEP < target ? target : mag - FADE_STEP;
    }
    layout(&L);
    hover_item = inside ? tile_at(&L, mx, my) : -1;
}

/* ---- repainting ---- */
static const char *label_of(int a){ return ui_apps[a]->name; }

static rect_t label_rect(const layout_t *L, int i){
    rect_t t = L->t[i];
    u32 w = ui_text_w(label_of(i), 1) + 24, h = 30;
    int x, y;
    if(!vertical()){ x = (int)t.x + (int)t.w / 2 - (int)w / 2; y = (int)t.y - 40; }
    else if(ui_prefs.dock_edge == DOCK_RIGHT){ x = (int)t.x - 14 - (int)w; y = (int)t.y + (int)t.h / 2 - 15; }
    else { x = (int)t.x + (int)t.w + 14; y = (int)t.y + (int)t.h / 2 - 15; }
    if(x < 4) x = 4;
    if(x + (int)w > scr_w - 4) x = scr_w - 4 - (int)w;
    return (rect_t){ (u32)x, (u32)y, w, h };
}

static rect_t bounds(const layout_t *L, int hover){
    int x0 = (int)L->pill.x, y0 = (int)L->pill.y, x1 = x0 + (int)L->pill.w, y1 = y0 + (int)L->pill.h;
    for(int i = 0; i < L->n; i++){
        rect_t t = L->t[i];
        if((int)t.x < x0) x0 = (int)t.x;
        if((int)t.y < y0) y0 = (int)t.y;
        if((int)(t.x + t.w) > x1) x1 = (int)(t.x + t.w);
        if((int)(t.y + t.h) > y1) y1 = (int)(t.y + t.h);
    }
    if(hover >= 0 && mag > 150){
        rect_t l = label_rect(L, hover);
        if((int)l.x < x0) x0 = (int)l.x;
        if((int)l.y < y0) y0 = (int)l.y;
        if((int)(l.x + l.w) > x1) x1 = (int)(l.x + l.w);
        if((int)(l.y + l.h) > y1) y1 = (int)(l.y + l.h);
    }
    return (rect_t){ (u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0) };
}

static u32 hash_words(const u32 *w, int n){
    u32 h = 2166136261u;
    for(int i = 0; i < n; i++){ h ^= w[i]; h *= 16777619u; }
    return h;
}

void dk_damage(u64 now){
    static rect_t sh_bounds;
    static u32 sh_sig;
    static int sh_on;
    (void)now;
    if(!ui_prefs.dock_on){
        if(sh_on){ gui_damage((int)sh_bounds.x - 4, (int)sh_bounds.y - 4, (int)sh_bounds.w + 14, (int)sh_bounds.h + 14); sh_on = 0; }
        return;
    }
    layout_t L; layout(&L);
    u32 w[UI_MAX_APPS + 8];
    int k = 0;
    int open = 0;
    for(int a = 0; a < L.n; a++){ if(win_is_open(a)) open |= 1 << a; }
    w[k++] = (u32)mag; w[k++] = (u32)(mag ? ptr_along : 0); w[k++] = (u32)(hover_item + 1);
    w[k++] = (u32)open; w[k++] = (u32)(win_top_app() + 2); w[k++] = (u32)ui_accent_index();
    w[k++] = (u32)(ui_prefs.dock_edge | ui_prefs.dock_size << 4 | ui_prefs.dock_zoom << 8 | L.n << 12);
    u32 sig = hash_words(w, k);
    if(!sh_on || sig != sh_sig){
        if(sh_on) gui_damage((int)sh_bounds.x - 4, (int)sh_bounds.y - 4, (int)sh_bounds.w + 14, (int)sh_bounds.h + 14);
        rect_t bd = bounds(&L, hover_item);
        gui_damage((int)bd.x - 4, (int)bd.y - 4, (int)bd.w + 14, (int)bd.h + 14);
        sh_bounds = bd; sh_sig = sig; sh_on = 1;
    }
}

void dk_draw(void){
    if(!ui_prefs.dock_on) return;
    layout_t L; layout(&L);
    rect_t bd = bounds(&L, hover_item);
    if(!fb_clip_hits((i32)bd.x - 4, (i32)bd.y - 4, (i32)bd.w + 14, (i32)bd.h + 14)) return;
    rect_t p = L.pill;
    u32 rad = (vertical() ? p.w : p.h) / 2;
    ui_rrect(p.x + 2, p.y + 5, p.w, p.h, rad, C_SHADOW);
    ui_panel(p.x, p.y, p.w, p.h, rad, C_BAR_EDGE, C_BAR);
    int top = win_top_app();
    for(int i = 0; i < L.n; i++){
        rect_t t = L.t[i];
        int open = win_is_open(i);
        ui_rrect(t.x, t.y, t.w, t.h, t.w * 28 / 100, i == hover_item ? C_HOVER : C_CARD);
        u32 col = i == top ? ui_accent() : open ? C_TEXT : C_MUTED;
        u32 sz = t.w * 62 / 100;
        ui_icon_scaled(ui_apps[i]->icon, t.x + (t.w - sz) / 2, t.y + (t.h - sz) / 2, sz, col);
        if(open){                                         /* a dot on the side of the screen edge */
            int dx, dy;
            if(!vertical()){ dx = (int)t.x + (int)t.w / 2; dy = (int)p.y + (int)p.h - 4; }
            else if(ui_prefs.dock_edge == DOCK_RIGHT){ dx = (int)p.x + (int)p.w - 4; dy = (int)t.y + (int)t.h / 2; }
            else { dx = (int)p.x + 4; dy = (int)t.y + (int)t.h / 2; }
            ui_disc(dx, dy, 2, i == top ? ui_accent() : C_MUTED);
        }
    }
    if(hover_item >= 0 && mag > 150){
        rect_t l = label_rect(&L, hover_item);
        ui_rrect(l.x, l.y, l.w, l.h, 8, C_MENU);
        ui_text(l.x + 12, l.y + 7, label_of(hover_item), C_TEXT, 1);
    }
}
