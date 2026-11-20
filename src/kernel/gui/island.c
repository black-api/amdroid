/* Dynamic Island: a pill that floats on the desktop.
 *  - Idle, it shows the system name, the clock or CPU/RAM (Settings picks), or shrinks to a
 *    thin handle.
 *  - A notification (ui_notify) grows it into a card with the app's icon, a title, a line
 *    of text and a bar that runs down while it stays. Several queue up and show one after
 *    another. Clicking a notification opens its app. With Do Not Disturb on, notifications
 *    go to the Notification Center only.
 *  - Clicked while idle, it opens to show memory, uptime, network and the last notifications.
 *  - It can be dragged anywhere (it snaps to the edges and the centre), and its size, style
 *    and timing are in Settings -> Dynamic Island. */
#include "gui_internal.h"
#include "amdroid/pmm.h"
#include "amdroid/net.h"
#include "amdroid/fb.h"
#include "amdroid/timer.h"

#define ISL_TOP     14
#define NOTE_QUEUE  8
#define NOTE_KEEP   3                  /* shown in the open island */
#define DRAG_SLOP   6                  /* pixels the pointer must travel before a press is a drag */
#define SNAP_DIST   26

typedef struct { int app; char title[40]; char body[72]; u64 at; } note_t;

static note_t queue[NOTE_QUEUE];
static int nqueue = 0;
static note_t cur;                     /* the notification on show */
static int showing = 0;
static u64 shown_at = 0;
static note_t hist[UI_HIST_MAX];       /* everything announced, newest first */
static int nhist = 0;

static int isl_w = 240, isl_h = 46;
static int isl_open = 0;
static rect_t box;
static u64 last_now = 0;
static int tgt_w = 240, tgt_h = 46;
static int scr_w = 0, scr_h = 0;

/* ---- the notification history (also read by the Notification Center) ---- */
int ui_hist_count(void){ return nhist; }
int ui_hist_get(int i, int *app, char *title, u32 tcap, char *body, u32 bcap, u64 *at){
    if(i < 0 || i >= nhist) return -1;
    if(app) *app = hist[i].app;
    if(title) ui_copy(title, hist[i].title, tcap);
    if(body) ui_copy(body, hist[i].body, bcap);
    if(at) *at = hist[i].at;
    return 0;
}
void ui_hist_remove(int i){
    if(i < 0 || i >= nhist) return;
    for(int k = i + 1; k < nhist; k++) hist[k - 1] = hist[k];
    nhist--;
}
void ui_hist_clear(void){ nhist = 0; }

void ui_notify(int app, const char *title, const char *body){
    note_t n;
    n.app = app;
    ui_copy(n.title, title ? title : "", sizeof n.title);
    ui_copy(n.body, body ? body : "", sizeof n.body);
    n.at = timer_ticks();
    if(nhist == UI_HIST_MAX) nhist--;                /* full: the oldest one goes */
    for(int i = nhist; i > 0; i--) hist[i] = hist[i - 1];
    hist[0] = n;
    nhist++;
    if(ui_prefs.dnd) return;                         /* Do Not Disturb: listed, not shown */
    if(nqueue == NOTE_QUEUE){                        /* full: the oldest waiting one goes */
        for(int i = 1; i < NOTE_QUEUE; i++) queue[i - 1] = queue[i];
        nqueue--;
    }
    queue[nqueue++] = n;
}

/* ---- looks, from the preferences ---- */
static const int IDLE_W[3] = { 190, 240, 300 }, IDLE_H[3] = { 40, 46, 54 }, CARD_W[3] = { 560, 640, 720 };
static int note_ticks(void){ return ui_prefs.isl_secs * 100; }
static int card_w(void){ return CARD_W[ui_prefs.isl_size]; }

typedef struct { u32 fill, edge, fg, muted, dim, rule, bar_back; int has_edge; } look_t;
static look_t look(void){
    if(ui_prefs.isl_style == ISL_STYLE_ACCENT)
        return (look_t){ ui_accent(), 0, 0x0B0F12, 0x1E2A33, 0x34444F, 0x2B3944, 0x4A6B7E, 0 };
    look_t l = { 0x000000, ui_accent(), C_TEXT, C_MUTED, C_DIM, 0x2A2F33, 0x22282C, 0 };
    if(ui_prefs.isl_style == ISL_STYLE_OUTLINE) l.has_edge = 1;
    return l;
}

/* The words on the resting pill. */
static void idle_text(char *out, u32 cap){
    out[0] = 0;
    if(ui_prefs.isl_idle == ISL_IDLE_CLOCK){ ui_clock(out); return; }
    if(ui_prefs.isl_idle == ISL_IDLE_STATS){
        char n[12], s[32] = "CPU ";
        ui_num(tb_cpu_tenths() / 10, n); ui_cat(s, n); ui_cat(s, "%  RAM ");
        ui_num(tb_ram_pct(), n); ui_cat(s, n); ui_cat(s, "%");
        ui_copy(out, s, cap);
        return;
    }
    if(ui_prefs.isl_idle == ISL_IDLE_NAME) ui_copy(out, "am-droid", cap);
}

/* One timer tick of easing towards the target size. */
static int step_to(int cur_v, int target){
    int d = target - cur_v;
    if(d == 0) return cur_v;
    int s = d / 6;
    if(s == 0) s = d > 0 ? 1 : -1;
    return cur_v + s;
}

static int open_height(void){ return 128 + 24 + (nhist ? (nhist > NOTE_KEEP ? NOTE_KEEP : nhist) * 56 : 40); }

/* ---- where it is ---- */
static int pos_cx(void){ return ui_prefs.isl_x >= 0 ? ui_prefs.isl_x : scr_w / 2; }
static int pos_top(void){ return ui_prefs.isl_y >= 0 ? ui_prefs.isl_y : ISL_TOP; }

/* ---- dragging ---- */
static int pressing = 0, dragging = 0;
static int press_x, press_y, grab_dx, grab_dy;

void isl_update(u32 sw, u64 now, int animate){
    scr_w = (int)sw;
    if(!scr_h) scr_h = (int)fb_height();
    u64 steps = now - last_now > 8 ? 8 : now - last_now;      /* ticks since the last frame */
    last_now = now;
    if(showing && now - shown_at >= (u64)note_ticks()) showing = 0;
    if(!showing && nqueue > 0 && !isl_open){
        cur = queue[0];
        for(int i = 1; i < nqueue; i++) queue[i - 1] = queue[i];
        nqueue--;
        showing = 1;
        shown_at = now;
    }
    int sz = ui_prefs.isl_size;
    int tw = IDLE_W[sz], th = IDLE_H[sz];
    if(ui_prefs.isl_idle == ISL_IDLE_HIDDEN){ tw = 64; th = 10; }
    if(isl_open){ tw = card_w(); th = open_height(); }
    else if(showing){ tw = card_w(); th = 88; }
    tgt_w = tw; tgt_h = th;
    if(animate){
        for(u64 k = 0; k < steps; k++){
            isl_w = step_to(isl_w, tw);
            isl_h = step_to(isl_h, th);
        }
    } else {
        isl_w = tw; isl_h = th;
    }
    int left = pos_cx() - isl_w / 2, top = pos_top();
    if(dragging){ left = ui_mouse_x() - grab_dx; top = ui_mouse_y() - grab_dy; }
    if(left < 8) left = 8;
    if(left > (int)sw - isl_w - 8) left = (int)sw - isl_w - 8;
    if(top < 6) top = 6;
    if(top > scr_h - isl_h - 6) top = scr_h - isl_h - 6;
    box = (rect_t){ (u32)left, (u32)top, (u32)isl_w, (u32)isl_h };
}

/* The icon of a notification: its app's, or a bell. */
static void note_icon(int app, u32 x, u32 y, u32 size, u32 tile, u32 glyph){
    ui_rrect(x, y, size, size, size / 2, tile);
    u32 ix = x + (size - 32) / 2, iy = y + (size - 32) / 2;
    if(app >= 0 && app < ui_app_count()) ui_icon(ui_apps[app]->icon, ix, iy, glyph);
    else {
        /* a bell */
        ui_fill((rect_t){ ix + 11, iy + 5, 10, 3 }, glyph);
        ui_fill((rect_t){ ix + 9, iy + 8, 14, 12 }, glyph);
        ui_fill((rect_t){ ix + 6, iy + 20, 20, 3 }, glyph);
        ui_fill((rect_t){ ix + 14, iy + 24, 4, 3 }, glyph);
    }
}

/* s cut to at most max characters (with ".." when cut), into out of cap bytes. */
static void fit(char *out, u32 cap, const char *s, u32 max){
    if(max + 1 > cap) max = cap - 1;
    u32 n = ui_len(s);
    ui_copy(out, s, max + 1);
    if(n > max && max > 3){ out[max - 2] = '.'; out[max - 1] = '.'; out[max] = 0; }
}

static int near(int a, int b){ return (a > b ? a - b : b - a) <= 6; }

/* What the island shows: 0 nothing yet (it is changing size), 1 its resting face, 2 a
 * notification, 3 the open panel. */
static int mode(void){
    int settled = near(isl_w, tgt_w) && near(isl_h, tgt_h);
    if(!settled) return 0;
    if(isl_open) return 3;
    if(showing) return 2;
    return 1;
}
/* The part of a notification's time bar that is left, in pixels. */
static u32 note_left(u64 now){
    u64 el = now - shown_at;
    u32 full = box.w - 120;
    u64 total = (u64)note_ticks();
    return el >= total ? 0 : (u32)((u64)full * (total - el) / total);
}

/* Redraw when the pill moves or grows, or when what it says changes. */
void isl_damage(u64 now){
    static rect_t sh_box;
    static int sh_mode = -1, sh_nhist = -1;
    static u32 sh_left;
    static u64 sh_sec, sh_note;
    static char sh_idle[32];
    int m = mode();
    u32 left = m == 2 ? note_left(now) : 0;
    u64 sec = m == 3 ? now / 100 : 0;                /* the open panel shows the uptime */
    char idle[32];
    if(m == 1) idle_text(idle, sizeof idle); else idle[0] = 0;
    if(box.x != sh_box.x || box.y != sh_box.y || box.w != sh_box.w || box.h != sh_box.h || m != sh_mode ||
       left != sh_left || sec != sh_sec || nhist != sh_nhist || shown_at != sh_note || !ui_streq(idle, sh_idle)){
        gui_damage((int)sh_box.x - 4, (int)sh_box.y - 4, (int)sh_box.w + 10, (int)sh_box.h + 14);
        gui_damage((int)box.x - 4, (int)box.y - 4, (int)box.w + 10, (int)box.h + 14);
        sh_box = box; sh_mode = m; sh_left = left; sh_sec = sec; sh_nhist = nhist; sh_note = shown_at;
        ui_copy(sh_idle, idle, sizeof sh_idle);
    }
}

void isl_draw(u64 now){
    rect_t b = box;
    if(!fb_clip_hits((i32)b.x - 4, (i32)b.y - 4, (i32)b.w + 10, (i32)b.h + 14)) return;
    look_t lk = look();
    u32 rad = b.h / 2 > 30 ? 30 : b.h / 2;
    ui_rrect(b.x + 2, b.y + 6, b.w, b.h, rad, C_SHADOW);
    if(lk.has_edge){
        ui_rrect(b.x, b.y, b.w, b.h, rad, lk.edge);
        if(b.w > 8 && b.h > 8) ui_rrect(b.x + 2, b.y + 2, b.w - 4, b.h - 4, rad > 2 ? rad - 2 : rad, lk.fill);
    } else ui_rrect(b.x, b.y, b.w, b.h, rad, lk.fill);
    u32 tile = ui_prefs.isl_style == ISL_STYLE_ACCENT ? 0x0B0F12 : ui_accent();
    u32 glyph = ui_prefs.isl_style == ISL_STYLE_ACCENT ? ui_accent() : C_BAR;

    int m = mode();
    if(m == 3){
        char up[12] = "", l1[64] = "", l2[64] = "", n[12];
        ui_uptime(up, now);
        u64 total = pmm_total_pages(), freep = pmm_free_pages();
        u32 pct = total ? (u32)((total - freep) * 100 / total) : 0;
        ui_cat(l1, "Memory "); ui_num(pct, n); ui_cat(l1, n); ui_cat(l1, "%   Up "); ui_cat(l1, up);
        if(net_up()){
            u32 ip = 0, gw = 0; u8 mac[6];
            net_info(&ip, &gw, mac);
            char s[24];
            ui_ip(s, ip);
            ui_cat(l2, "Net "); ui_cat(l2, s);
        } else {
            ui_cat(l2, "Network offline");
        }
        ui_text(b.x + (b.w - ui_text_w(l1, UI_SCALE)) / 2, b.y + 18, l1, lk.fg, UI_SCALE);
        ui_text(b.x + (b.w - ui_text_w(l2, UI_SCALE)) / 2, b.y + 60, l2, lk.muted, UI_SCALE);
        ui_fill((rect_t){ b.x + 32, b.y + 108, b.w - 64, 1 }, lk.rule);
        u32 y = b.y + 124;
        int shown = nhist > NOTE_KEEP ? NOTE_KEEP : nhist;
        if(!shown) ui_text(b.x + 32, y, ui_prefs.dnd ? "Do Not Disturb is on" : "No notifications", lk.dim, 1);
        u32 maxc = (b.w - 88 - 32) / 8;
        for(int i = 0; i < shown; i++){
            note_icon(hist[i].app, b.x + 32, y, 40, tile, glyph);
            char t[80];
            fit(t, sizeof t, hist[i].title, maxc);
            ui_text(b.x + 88, y + 2, t, lk.fg, 1);
            fit(t, sizeof t, hist[i].body, maxc);
            ui_text(b.x + 88, y + 22, t, lk.muted, 1);
            y += 56;
        }
    } else if(m == 2){
        note_icon(cur.app, b.x + 20, b.y + 16, 56, tile, glyph);
        char t[48];
        fit(t, sizeof t, cur.title, (b.w - 92 - 28) / 16);
        ui_text(b.x + 92, b.y + (cur.body[0] ? 12 : 28), t, lk.fg, UI_SCALE);
        if(cur.body[0]){
            char bd[80];
            fit(bd, sizeof bd, cur.body, (b.w - 92 - 28) / 8);
            ui_text(b.x + 92, b.y + 50, bd, lk.muted, 1);
        }
        /* the time it has left, as a bar that shrinks */
        u32 full = b.w - 120;
        u32 left = note_left(now);
        ui_fill((rect_t){ b.x + 92, b.y + b.h - 10, full - 20, 2 }, lk.bar_back);
        if(left > 20) ui_fill((rect_t){ b.x + 92, b.y + b.h - 10, left - 20, 2 },
                              ui_prefs.isl_style == ISL_STYLE_ACCENT ? 0x0B0F12 : ui_accent());
    } else if(m == 1 && ui_prefs.isl_idle != ISL_IDLE_HIDDEN){
        char t[32];
        idle_text(t, sizeof t);
        u32 tw = ui_text_w(t, UI_SCALE);
        ui_text(b.x + (b.w - tw) / 2, b.y + (b.h - UI_CH) / 2, t, lk.fg, UI_SCALE);
        if(ui_prefs.dnd){                             /* a small moon: Do Not Disturb */
            u32 mx = b.x + 20, my = b.y + b.h / 2;
            ui_disc((int)mx, (int)my, 7, lk.fg);
            ui_disc((int)mx + 4, (int)my - 3, 6, lk.fill);
        }
    }
}

int isl_hit(int x, int y){
    rect_t r = box;
    if(r.h < 14){ r.y -= 4; r.h += 8; }              /* the thin handle is hard to hit */
    return ui_contains(r, x, y);
}

/* A press on the island: it may turn into a drag, or a click when released. */
void isl_press(int x, int y){
    pressing = 1; dragging = 0;
    press_x = x; press_y = y;
    grab_dx = x - (int)box.x; grab_dy = y - (int)box.y;
}

/* Where a dragged pill settles: near an edge or the centre it snaps, within the screen. */
static void settle(void){
    int w = (int)box.w, left = (int)box.x, top = (int)box.y;
    int cx = left + w / 2;
    int iw = ui_prefs.isl_idle == ISL_IDLE_HIDDEN ? 64 : IDLE_W[ui_prefs.isl_size];   /* what it shrinks to */
    if(ui_prefs.isl_snap){
        if(cx > scr_w / 2 - SNAP_DIST && cx < scr_w / 2 + SNAP_DIST) cx = scr_w / 2;
        if(left < SNAP_DIST + 14) cx = 14 + iw / 2;
        if(scr_w - (left + w) < SNAP_DIST + 14) cx = scr_w - 14 - iw / 2;
        if(top < SNAP_DIST + ISL_TOP) top = ISL_TOP;
    }
    ui_prefs.isl_x = cx;
    ui_prefs.isl_y = top;
}

/* Every GUI turn: follows the pointer while it is dragged and, when the button comes up,
 * finishes the click or the drag. Returns the app a clicked notification opens, or -1. */
int isl_drag(int mx, int my, int held){
    if(!pressing) return -1;
    if(held){
        int dx = mx - press_x, dy = my - press_y;
        if(!dragging && (dx > DRAG_SLOP || dx < -DRAG_SLOP || dy > DRAG_SLOP || dy < -DRAG_SLOP)) dragging = 1;
        return -1;
    }
    pressing = 0;
    if(dragging){
        dragging = 0;
        settle();
        prefs_touch();
        return -1;
    }
    return isl_click();
}
int isl_dragging(void){ return dragging; }

/* Back to the top centre. */
void isl_reset_position(void){
    ui_prefs.isl_x = ui_prefs.isl_y = -1;
}

/* A click on the island. Returns the app to open (a clicked notification), or -1. */
int isl_click(void){
    if(showing && !isl_open){
        showing = 0;                                  /* dismissed */
        return cur.app;
    }
    isl_open = !isl_open;
    return -1;
}

void isl_close(void){ isl_open = 0; }
void isl_toggle_open(void){ isl_open = !isl_open; }
