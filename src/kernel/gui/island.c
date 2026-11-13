/* Dynamic Island: a black pill at the top centre.
 *  - Idle, it shows the system name.
 *  - A notification (ui_notify) grows it into a card with the app's icon, a title, a line
 *    of text and a bar that runs down for the 4 seconds it stays. Several queue up and show
 *    one after another. Clicking a notification opens its app.
 *  - Clicked while idle, it opens to show memory, uptime, network and the last notifications. */
#include "gui_internal.h"
#include "amdroid/pmm.h"
#include "amdroid/net.h"
#include "amdroid/fb.h"

#define ISL_TOP     14
#define NOTE_TICKS  400                /* 4 seconds on screen */
#define NOTE_QUEUE  8
#define NOTE_KEEP   3                  /* shown in the open island */

typedef struct { int app; char title[40]; char body[72]; u64 at; } note_t;

static note_t queue[NOTE_QUEUE];
static int nqueue = 0;
static note_t cur;                     /* the notification on show */
static int showing = 0;
static u64 shown_at = 0;
static note_t recent[NOTE_KEEP];       /* newest first */
static int nrecent = 0;

static int isl_w = 240, isl_h = 44;
static int isl_open = 0;
static rect_t box;
static u64 last_now = 0;

void ui_notify(int app, const char *title, const char *body){
    if(nqueue == NOTE_QUEUE){                       /* full: the oldest waiting one goes */
        for(int i = 1; i < NOTE_QUEUE; i++) queue[i - 1] = queue[i];
        nqueue--;
    }
    note_t *n = &queue[nqueue++];
    n->app = app;
    ui_copy(n->title, title ? title : "", sizeof n->title);
    ui_copy(n->body, body ? body : "", sizeof n->body);
    n->at = last_now;
}

/* One timer tick of easing towards the target size. */
static int step_to(int cur_v, int target){
    int d = target - cur_v;
    if(d == 0) return cur_v;
    int s = d / 6;
    if(s == 0) s = d > 0 ? 1 : -1;
    return cur_v + s;
}

static int open_height(void){ return 128 + 24 + (nrecent ? nrecent * 56 : 40); }

void isl_update(u32 sw, u64 now, int animate){
    u64 steps = now - last_now > 8 ? 8 : now - last_now;      /* ticks since the last frame */
    last_now = now;
    if(showing && now - shown_at >= NOTE_TICKS) showing = 0;
    if(!showing && nqueue > 0 && !isl_open){
        cur = queue[0];
        for(int i = 1; i < nqueue; i++) queue[i - 1] = queue[i];
        nqueue--;
        showing = 1;
        shown_at = now;
        for(int i = NOTE_KEEP - 1; i > 0; i--) recent[i] = recent[i - 1];
        recent[0] = cur;
        if(nrecent < NOTE_KEEP) nrecent++;
    }
    int tw = 240, th = 44;
    if(isl_open){ tw = 640; th = open_height(); }
    else if(showing){ tw = 640; th = 88; }
    if(animate){
        for(u64 k = 0; k < steps; k++){
            isl_w = step_to(isl_w, tw);
            isl_h = step_to(isl_h, th);
        }
    } else {
        isl_w = tw; isl_h = th;
    }
    box = (rect_t){ (u32)((int)sw - isl_w) / 2, ISL_TOP, (u32)isl_w, (u32)isl_h };
}

/* The icon of a notification: its app's, or a bell. */
static void note_icon(int app, u32 x, u32 y, u32 size){
    u32 tile = ui_accent();
    ui_rrect(x, y, size, size, size / 2, tile);
    u32 ix = x + (size - 32) / 2, iy = y + (size - 32) / 2;
    if(app >= 0 && app < ui_app_count()) ui_icon(ui_apps[app]->icon, ix, iy, C_BAR);
    else {
        /* a bell */
        ui_fill((rect_t){ ix + 11, iy + 5, 10, 3 }, C_BAR);
        ui_fill((rect_t){ ix + 9, iy + 8, 14, 12 }, C_BAR);
        ui_fill((rect_t){ ix + 6, iy + 20, 20, 3 }, C_BAR);
        ui_fill((rect_t){ ix + 14, iy + 24, 4, 3 }, C_BAR);
    }
}

/* s cut to at most max characters (with ".." when cut), into out of cap bytes. */
static void fit(char *out, u32 cap, const char *s, u32 max){
    if(max + 1 > cap) max = cap - 1;
    u32 n = ui_len(s);
    ui_copy(out, s, max + 1);
    if(n > max && max > 3){ out[max - 2] = '.'; out[max - 1] = '.'; out[max] = 0; }
}

/* What the island shows: 0 nothing yet (it is changing size), 1 its name, 2 a notification,
 * 3 the open panel. */
static int mode(void){
    if(isl_open && isl_w >= 620 && isl_h >= open_height() - 6) return 3;
    if(!isl_open && showing && isl_w >= 620 && isl_h >= 82) return 2;
    if(!isl_open && !showing && isl_w <= 260) return 1;
    return 0;
}
/* The part of a notification's time bar that is left, in pixels. */
static u32 note_left(u64 now){
    u64 el = now - shown_at;
    u32 full = box.w - 120;
    return el >= NOTE_TICKS ? 0 : (u32)((u64)full * (NOTE_TICKS - el) / NOTE_TICKS);
}

/* Redraw when the pill moves or grows, or when what it says changes. */
void isl_damage(u64 now){
    static rect_t sh_box;
    static int sh_mode = -1, sh_nrecent = -1;
    static u32 sh_left;
    static u64 sh_sec, sh_note;
    int m = mode();
    u32 left = m == 2 ? note_left(now) : 0;
    u64 sec = m == 3 ? now / 100 : 0;                /* the open panel shows the uptime */
    if(box.x != sh_box.x || box.w != sh_box.w || box.h != sh_box.h || m != sh_mode || left != sh_left ||
       sec != sh_sec || nrecent != sh_nrecent || shown_at != sh_note){
        gui_damage((int)sh_box.x, (int)sh_box.y, (int)sh_box.w + 2, (int)sh_box.h + 6);
        gui_damage((int)box.x, (int)box.y, (int)box.w + 2, (int)box.h + 6);
        sh_box = box; sh_mode = m; sh_left = left; sh_sec = sec; sh_nrecent = nrecent; sh_note = shown_at;
    }
}

void isl_draw(u64 now){
    rect_t b = box;
    if(!fb_clip_hits((i32)b.x, (i32)b.y, (i32)b.w + 2, (i32)b.h + 6)) return;
    ui_rrect(b.x + 2, b.y + 6, b.w, b.h, b.h / 2 > 30 ? 30 : b.h / 2, C_SHADOW);
    ui_rrect(b.x, b.y, b.w, b.h, b.h / 2 > 30 ? 30 : b.h / 2, 0x000000);

    int settled_open = mode() == 3;
    int settled_note = mode() == 2;

    if(settled_open){
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
        ui_text(b.x + (b.w - ui_text_w(l1, UI_SCALE)) / 2, b.y + 18, l1, C_TEXT, UI_SCALE);
        ui_text(b.x + (b.w - ui_text_w(l2, UI_SCALE)) / 2, b.y + 60, l2, C_MUTED, UI_SCALE);
        ui_fill((rect_t){ b.x + 32, b.y + 108, b.w - 64, 1 }, 0x2A2F33);
        u32 y = b.y + 124;
        if(!nrecent) ui_text(b.x + 32, y, "No notifications", C_DIM, 1);
        for(int i = 0; i < nrecent; i++){
            note_icon(recent[i].app, b.x + 32, y, 40);
            char t[80];
            fit(t, sizeof t, recent[i].title, 34);
            ui_text(b.x + 88, y + 2, t, C_TEXT, 1);
            fit(t, sizeof t, recent[i].body, 64);
            ui_text(b.x + 88, y + 22, t, C_MUTED, 1);
            y += 56;
        }
    } else if(settled_note){
        note_icon(cur.app, b.x + 20, b.y + 16, 56);
        char t[48];
        fit(t, sizeof t, cur.title, 30);
        ui_text(b.x + 92, b.y + (cur.body[0] ? 12 : 28), t, C_TEXT, UI_SCALE);
        if(cur.body[0]){
            char bd[80];
            fit(bd, sizeof bd, cur.body, 64);
            ui_text(b.x + 92, b.y + 50, bd, C_MUTED, 1);
        }
        /* the time it has left, as a bar that shrinks */
        u32 full = b.w - 120;
        u32 left = note_left(now);
        ui_fill((rect_t){ b.x + 92, b.y + b.h - 10, full - 20, 2 }, 0x22282C);
        if(left > 20) ui_fill((rect_t){ b.x + 92, b.y + b.h - 10, left - 20, 2 }, ui_accent());
    } else if(mode() == 1){
        const char *t = "am-droid";
        ui_text(b.x + (b.w - ui_text_w(t, UI_SCALE)) / 2, b.y + (b.h - UI_CH) / 2, t, C_TEXT, UI_SCALE);
    }
}

int isl_hit(int x, int y){ return ui_contains(box, x, y); }

/* A click on the island. Returns the app to open (a clicked notification), or -1. */
int isl_click(void){
    if(showing && !isl_open){
        showing = 0;                                  /* dismissed */
        return cur.app;
    }
    isl_open = !isl_open;
    return -1;
}
