/* Window manager: placement, stacking, focus, dragging and the chrome around each app. */
#include "gui_internal.h"
#include "amdroid/fb.h"
#include "amdroid/timer.h"

static rect_t rects[UI_MAX_APPS];
static int is_open[UI_MAX_APPS];
static int is_min[UI_MAX_APPS];
static u64 start[UI_MAX_APPS];          /* open animation start tick */
static int zord[UI_MAX_APPS], zn = 0;  /* bottom .. top */
static int cascade = 0;
static int scr_w = 0, scr_h = 0;

int win_drag_app = -1, win_drag_dx = 0, win_drag_dy = 0;
int win_snap_zone = 0;                  /* while dragging: 1 left half, 2 right half, 3 maximize */

static rect_t saved[UI_MAX_APPS];       /* the size before maximizing or snapping */
static int is_max[UI_MAX_APPS];         /* 0 normal, 1 maximized, 2 snapped to a half */
static rect_t remembered[UI_MAX_APPS];  /* where each app was when it was last closed */
static int has_mem[UI_MAX_APPS];
static int ptr_x = -1, ptr_y = -1;      /* the pointer, for hover on the title buttons */

void win_set_pointer(int x, int y){ ptr_x = x; ptr_y = y; }
int  win_is_max(int a){ return is_max[a]; }

/* The screen above the taskbar: what maximized and snapped windows fill. */
static rect_t work_area(void){
    /* Below the Dynamic Island (top 14 + height 44 + gap) and above the taskbar. */
    return (rect_t){ 0, 72, (u32)scr_w, (u32)(scr_h - 48 - 2 * 12 - 8 - 72) };
}

static int clampi_(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }

int win_at(int x, int y){
    for(int i = zn - 1; i >= 0; i--){
        int a = zord[i];
        if(is_open[a] && !is_min[a] && ui_contains(rects[a], x, y)) return a;
    }
    return -1;
}

int    win_is_open(int a){ return is_open[a]; }
int    win_is_min(int a){ return is_min[a]; }
rect_t win_rect_of(int a){ return rects[a]; }

rect_t win_body(int a){
    rect_t r = rects[a];
    return (rect_t){ r.x + UI_PAD, r.y + UI_TITLE_H + 12, r.w - 2 * UI_PAD, r.h - UI_TITLE_H - 12 - UI_PAD };
}
rect_t win_close_button(int a){
    rect_t r = rects[a];
    return (rect_t){ r.x + r.w - 44, r.y + 6, 36, 36 };
}
rect_t win_max_button(int a){
    rect_t r = rects[a];
    return (rect_t){ r.x + r.w - 84, r.y + 6, 36, 36 };
}
rect_t win_min_button(int a){
    rect_t r = rects[a];
    return (rect_t){ r.x + r.w - 124, r.y + 6, 36, 36 };
}

void win_toggle_max(int a){
    if(is_max[a]){ rects[a] = saved[a]; is_max[a] = 0; return; }
    saved[a] = rects[a];
    rects[a] = work_area();
    is_max[a] = 1;
}

/* zone 1 = left half, 2 = right half, 3 = the whole work area */
void win_snap(int a, int zone){
    if(zone < 1 || zone > 3) return;
    if(!is_max[a]) saved[a] = rects[a];
    rect_t w = work_area();
    if(zone == 3){ rects[a] = w; is_max[a] = 1; return; }
    rects[a] = (rect_t){ zone == 1 ? 0 : w.w / 2, w.y, w.w / 2, w.h };
    is_max[a] = 2;
}

/* Starting to drag a maximized or snapped window gives it back its old size under the pointer.
 * Returns the new horizontal grab offset. */
int win_unsnap_for_drag(int a, int mx){
    if(!is_max[a]) return mx - (int)rects[a].x;
    rect_t old = saved[a];
    int x = mx - (int)old.w / 2;
    if(x < 0) x = 0;
    if(x > scr_w - (int)old.w) x = scr_w - (int)old.w;
    rects[a] = (rect_t){ (u32)x, rects[a].y, old.w, old.h };
    is_max[a] = 0;
    return mx - x;
}

static void zremove(int a){
    int j = 0;
    for(int i = 0; i < zn; i++) if(zord[i] != a) zord[j++] = zord[i];
    zn = j;
}
static void zpush(int a){
    zremove(a);
    zord[zn++] = a;
}
int win_top_app(void){
    for(int i = zn - 1; i >= 0; i--)
        if(is_open[zord[i]] && !is_min[zord[i]]) return zord[i];
    return -1;
}

/* A window size and place that depends on the screen; cascades a little. */
static void place(int a){
    u32 w = (u32)scr_w * 5 / 10;
    if(w < 720) w = 720;
    if(w > 980) w = 980;
    u32 avail = (u32)scr_h - 48 - 2 * 12 - 24;      /* screen minus taskbar and margins */
    u32 h = avail * 85 / 100;
    if(h < 560) h = avail < 560 ? avail : 560;
    if(h > 620) h = 620;
    int off = cascade * 36;
    cascade = (cascade + 1) % 5;
    int x = (scr_w - (int)w) / 2 + off - 72;
    int y = ((int)avail - (int)h) / 2 + off - 24;
    rects[a] = (rect_t){ (u32)clampi_(x, 0, scr_w - (int)w), (u32)clampi_(y, 0, (int)avail), w, h };
}

void win_init(u32 sw, u32 sh){
    scr_w = (int)sw; scr_h = (int)sh;
    zn = 0;
    for(int a = 0; a < UI_MAX_APPS; a++){ is_open[a] = 0; is_min[a] = 0; }
}

/* A window that is closing or minimizing keeps sliding for a moment after it is gone. */
#define GHOST_TICKS 14
typedef struct { int on; int dy; u64 start; rect_t r; } ghost_t;
static ghost_t ghost[UI_MAX_APPS];

void win_open_app(int a, u64 now){
    if(a < 0 || a >= ui_app_count()) return;
    ghost[a].on = 0;
    if(!is_open[a]){
        if(has_mem[a]) rects[a] = remembered[a];   /* where it was last time */
        else place(a);
        is_max[a] = 0;
        is_open[a] = 1;
        start[a] = now;
        if(ui_apps[a]->open){ ui_app_enter(a); ui_apps[a]->open(); ui_app_leave(); }
    }
    is_min[a] = 0;
    zpush(a);
}

static void start_ghost(int a, int dy){
    ghost[a].on = ui_anim_on;
    ghost[a].dy = dy;
    ghost[a].start = timer_ticks();
    ghost[a].r = rects[a];
}

void win_close_app(int a){
    if(!is_open[a]) return;
    start_ghost(a, 24);
    remembered[a] = is_max[a] ? saved[a] : rects[a];
    has_mem[a] = 1;
    is_open[a] = 0;
    is_min[a] = 0;
    zremove(a);
    if(ui_apps[a]->close){ ui_app_enter(a); ui_apps[a]->close(); ui_app_leave(); }
    if(win_drag_app == a) win_drag_app = -1;
}
void win_minimize(int a){
    if(is_min[a] || !is_open[a]) return;
    start_ghost(a, 140);
    is_min[a] = 1;
}
void win_focus(int a){
    if(is_open[a]){ is_min[a] = 0; zpush(a); }
}
void win_cycle_focus(void){
    int cnt = 0;
    for(int i = 0; i < zn; i++) if(is_open[zord[i]] && !is_min[zord[i]]) cnt++;
    if(cnt > 1) zpush(zord[0]);
}
void win_set_size(int a, int w, int h){
    if(w < WIN_MIN_W) w = WIN_MIN_W;
    if(h < WIN_MIN_H) h = WIN_MIN_H;
    if(w > scr_w) w = scr_w;
    if(h > scr_h) h = scr_h;
    rects[a].w = (u32)w;
    rects[a].h = (u32)h;
}

rect_t win_grip(int a){
    rect_t r = rects[a];
    return (rect_t){ r.x + r.w - 22, r.y + r.h - 22, 22, 22 };
}

void win_move(int a, int x, int y){
    rects[a].x = (u32)x;
    rects[a].y = (u32)y;
}

/* The window frame: shadow, surface, title and close mark. The body is drawn only when
 * the app is shown (not for a closing ghost). */
static void chrome(int a, rect_t r, int focused, int with_body){
    ui_rrect(r.x + 6, r.y + 10, r.w, r.h, 16, C_SHADOW);
    ui_rrect(r.x, r.y, r.w, r.h, 16, C_WIN);
    ui_rrect(r.x, r.y, r.w, 1, 0, C_WIN_EDGE);

    const app_t *app = ui_apps[a];
    ui_icon(app->icon, r.x + 14, r.y + (UI_TITLE_H - 32) / 2, focused ? ui_accent() : C_MUTED);
    ui_text(r.x + 56, r.y + (UI_TITLE_H - UI_CH) / 2, app->name,
            focused ? C_TEXT : C_MUTED, UI_SCALE);
    ui_fill((rect_t){ r.x + 1, r.y + UI_TITLE_H, r.w - 2, 1 }, C_SEP);
    /* minimize, maximize/restore and close, with hover */
    for(int k = 0; k < 3; k++){
        rect_t bt = { r.x + r.w - 124 + (u32)k * 40, r.y + 6, 36, 36 };
        int hot = ptr_x >= (int)bt.x && ptr_x < (int)(bt.x + bt.w) && ptr_y >= (int)bt.y && ptr_y < (int)(bt.y + bt.h);
        if(hot) ui_rrect(bt.x, bt.y, bt.w, bt.h, 8, k == 2 ? 0xC42B1C : C_HOVER);
        u32 gc = hot ? C_TEXT : C_MUTED;
        u32 cx = bt.x + bt.w / 2, cy = bt.y + bt.h / 2;
        if(k == 0){
            ui_fill((rect_t){ cx - 7, cy + 3, 14, 2 }, gc);
        } else if(k == 1){
            if(is_max[a] == 1){
                /* restore: two overlapping squares */
                ui_fill((rect_t){ cx - 4, cy - 7, 11, 2 }, gc); ui_fill((rect_t){ cx + 5, cy - 7, 2, 11 }, gc);
                rect_t q = { cx - 7, cy - 3, 11, 11 };
                ui_fill((rect_t){ q.x, q.y, q.w, 2 }, gc); ui_fill((rect_t){ q.x, q.y + q.h - 2, q.w, 2 }, gc);
                ui_fill((rect_t){ q.x, q.y, 2, q.h }, gc); ui_fill((rect_t){ q.x + q.w - 2, q.y, 2, q.h }, gc);
            } else {
                rect_t q = { cx - 7, cy - 7, 14, 14 };
                ui_fill((rect_t){ q.x, q.y, q.w, 2 }, gc); ui_fill((rect_t){ q.x, q.y + q.h - 2, q.w, 2 }, gc);
                ui_fill((rect_t){ q.x, q.y, 2, q.h }, gc); ui_fill((rect_t){ q.x + q.w - 2, q.y, 2, q.h }, gc);
            }
        } else {
            /* close: an X made of small squares along both diagonals */
            for(int d = -6; d <= 6; d++){
                ui_fill((rect_t){ (u32)((int)cx + d - 1), (u32)((int)cy + d - 1), 3, 3 }, gc);
                ui_fill((rect_t){ (u32)((int)cx + d - 1), (u32)((int)cy - d - 1), 3, 3 }, gc);
            }
        }
    }

    /* the resize corner */
    ui_fill((rect_t){ r.x + r.w - 8, r.y + r.h - 8, 3, 3 }, C_MUTED);
    ui_fill((rect_t){ r.x + r.w - 13, r.y + r.h - 8, 3, 3 }, C_MUTED);
    ui_fill((rect_t){ r.x + r.w - 8, r.y + r.h - 13, 3, 3 }, C_MUTED);
    if(with_body && app->draw){
        rect_t b = { r.x + UI_PAD, r.y + UI_TITLE_H + 12, r.w - 2 * UI_PAD, r.h - UI_TITLE_H - 12 - UI_PAD };
        ui_app_enter(a);
        app->draw(b, focused);
        ui_app_leave();
    }
}

/* How far down a window is pushed by its open, close or minimize animation. */
static int slide_offset(u64 now, u64 from, int ticks, int dy, int down){
    if(!ui_anim_on) return 0;
    int t = (int)((now - from) * 255 / (u64)ticks);
    if(t >= 255) return 0;
    int e = ui_ease(t);                 /* 0..255, rising */
    return down ? (e * dy) / 255 : ((255 - e) * dy) / 255;
}

void win_draw_all(u64 now, int animate){
    (void)animate;   /* the switch is read from ui_anim_on in slide_offset */
    int top = win_top_app();
    for(int i = 0; i < zn; i++){
        int a = zord[i];
        if(!is_open[a] || is_min[a]) continue;
        rect_t r = rects[a];
        if(ui_anim_on && now - start[a] < WIN_ANIM_TICKS)
            r.y += (u32)(((255 - ui_ease((int)((now - start[a]) * 255 / WIN_ANIM_TICKS))) * 48) / 255);
        chrome(a, r, a == top, 1);
    }
    if(win_snap_zone){
        /* where the dragged window will land if it is let go here */
        rect_t w = work_area();
        rect_t p = win_snap_zone == 3 ? w : (rect_t){ win_snap_zone == 1 ? 0 : w.w / 2, w.y, w.w / 2, w.h };
        p.x += 8; p.y += 8; p.w -= 16; p.h -= 16;
        u32 c = ui_accent();
        ui_fill((rect_t){ p.x, p.y, p.w, 3 }, c); ui_fill((rect_t){ p.x, p.y + p.h - 3, p.w, 3 }, c);
        ui_fill((rect_t){ p.x, p.y, 3, p.h }, c); ui_fill((rect_t){ p.x + p.w - 3, p.y, 3, p.h }, c);
    }
    for(int a = 0; a < ui_app_count(); a++){
        if(!ghost[a].on) continue;
        if(now - ghost[a].start >= GHOST_TICKS){ ghost[a].on = 0; continue; }
        int dy = slide_offset(now, ghost[a].start, GHOST_TICKS, ghost[a].dy, 1);
        rect_t r = ghost[a].r;
        r.y += (u32)dy;
        chrome(a, r, 0, 0);
    }
}

/* For apps (the task manager). */
int  ui_app_is_open(int a){ return a >= 0 && a < UI_MAX_APPS && win_is_open(a); }
void ui_app_close(int a){ if(ui_app_is_open(a)) win_close_app(a); }
