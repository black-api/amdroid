/* The GUI main loop: input routing, drawing order and the cursor. */
#include "amdroid/gui.h"
#include "amdroid/fb.h"
#include "amdroid/mouse.h"
#include "amdroid/keyboard.h"
#include "amdroid/serial.h"
#include "amdroid/timer.h"
#include "amdroid/console.h"
#include "amdroid/auth.h"
#include "amdroid/net.h"
#include "amdroid/sched.h"
#include "amdroid/pkg.h"
#include "amdroid/fuss.h"
#include "gui_internal.h"

static int start_open = 0;
static int rs_app = -1, rs_gx = 0, rs_gy = 0;      /* a window being resized, and the grab offset */
static int last_title_app = -1;                    /* for double-clicks on a title bar */
static u64 last_title_at = 0;
static int drag_px = 0, drag_py = 0;               /* where a title-bar drag started */
static u64 start_opened_at = 0;      /* when the Start menu opened, for its slide */
static int hov_item = -1;
static u64 hov_since = 0;            /* when the pointer came onto that taskbar item */

static void set_start(int open, u64 now){
    if(open && !start_open){ start_opened_at = now; tb_menu_reset(); }
    start_open = open;
}

/* ---- repainting ----
 * Only the parts of the screen that changed are drawn again. Each layer reports what
 * changed (gui_damage); the frame then redraws those rectangles from the wallpaper up and
 * copies just them to the screen. With nothing reported, nothing is drawn at all. */
#define DMG_MAX 12
typedef struct { int x0, y0, x1, y1; } box_t;
static box_t dmg[DMG_MAX];
static int ndmg = 0;
static int scr_w = 0, scr_h = 0;

static long box_area(box_t b){ return (long)(b.x1 - b.x0) * (b.y1 - b.y0); }
static box_t box_union(box_t a, box_t b){
    return (box_t){ a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                    a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
}

void gui_damage(int x, int y, int w, int h){
    box_t b = { x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > scr_w ? scr_w : x + w, y + h > scr_h ? scr_h : y + h };
    if(b.x1 <= b.x0 || b.y1 <= b.y0) return;
    /* boxes that overlap or nearly touch become one, so no pixel is drawn twice for nothing */
    for(int again = 1; again; ){
        again = 0;
        for(int i = 0; i < ndmg; i++){
            box_t u = box_union(dmg[i], b);
            if(box_area(u) <= box_area(dmg[i]) + box_area(b) + 4096){
                b = u;
                dmg[i] = dmg[--ndmg];
                again = 1;
                break;
            }
        }
    }
    if(ndmg == DMG_MAX){                      /* no room left: one box around them all */
        for(int i = 0; i < ndmg; i++) b = box_union(b, dmg[i]);
        ndmg = 0;
    }
    dmg[ndmg++] = b;
}

void gui_damage_all(void){
    ndmg = 0;
    dmg[ndmg++] = (box_t){ 0, 0, scr_w, scr_h };
}

/* ---- how the screen is doing, for the Tasks app: measured over the last full second ---- */
static ui_perf_t perf, perf_run;
static u64 perf_sec = 0, perf_us_sum = 0, perf_px_sum = 0;
static u64 cal_tsc = 0, cal_tick = 0, cyc_per_tick = 0;

void ui_perf(ui_perf_t *out){ *out = perf; }

static void perf_frame(u64 cycles, u64 pixels){
    u32 us = cyc_per_tick ? (u32)(cycles * 10000 / cyc_per_tick) : 0;
    perf_run.redraws++;
    perf_us_sum += us;
    perf_px_sum += pixels;
    if(us > perf_run.max_us) perf_run.max_us = us;
}

static void perf_loop(u64 now){
    u64 tsc = sched_tsc();
    if(!cal_tick){ cal_tick = now; cal_tsc = tsc; }
    else if(now - cal_tick >= 50){               /* how many CPU cycles a timer tick lasts */
        cyc_per_tick = (tsc - cal_tsc) / (now - cal_tick);
        cal_tick = now; cal_tsc = tsc;
    }
    perf_run.loops++;
    if(now / 100 != perf_sec){
        perf_sec = now / 100;
        perf = perf_run;
        perf.avg_us = perf.redraws ? (u32)(perf_us_sum / perf.redraws) : 0;
        perf.screen_tenths = (u32)(perf_px_sum * 1000 / ((u64)scr_w * (u64)scr_h * 100));
#ifdef GUI_PERF_LOG
        console_printf("[perf] loops %u redraws %u avg %u us max %u us screen %u/10\n",
                       perf.loops, perf.redraws, perf.avg_us, perf.max_us, perf.screen_tenths);
#endif
        perf_run = (ui_perf_t){ 0 };
        perf_us_sum = perf_px_sum = 0;
    }
}

/* Starts an app from the taskbar, the Start menu or a key. Terminal leaves the GUI. */
/* Runs an installed .adp app full screen until it exits, then repaints. */
void gui_run_pkg(const char *id){
    char path[64];
    if(pkg_entry_path(id, path, sizeof path) != 0) return;
    mouse_event_t ev; while(mouse_poll(&ev)) { }      /* swallow the launching click */
    fuss_run_file(path);
    while(mouse_poll(&ev)) { }                         /* and the exiting click */
    gui_damage_all();
}

static void launch(int a, u64 now, int *next_vt){
    if(a == UI_MAX_APPS){ *next_vt = 2; return; }     /* Terminal */
    if(a == UI_MAX_APPS + 1){ *next_vt = 3; return; }  /* Sign out: back to the login screen */
    const char *pid = tb_menu_pkg(a);                  /* an installed app from the Start menu? */
    if(pid){ start_open = 0; gui_run_pkg(pid); return; }
    win_open_app(a, now);
    start_open = 0;
}

/* Leaving for good (sign out): every app closes properly, so Notes saves and the
 * browser's renderer goes away. */
static void close_all(void){
    for(int a = 0; a < ui_app_count(); a++) if(win_is_open(a)) win_close_app(a);
}

static void run_item(item_t it, u64 now, int *next_vt){
    if(it.kind == IT_START || it.kind == IT_SEARCH){ set_start(!start_open, now); return; }
    if(it.kind == IT_TERM){ launch(UI_MAX_APPS, now, next_vt); return; }
    int a = it.app;
    if(win_is_open(a) && !win_is_min(a) && win_top_app() == a){ win_minimize(a); return; }
    launch(a, now, next_vt);
}

/* ---- right-click menus ---- */

enum { A_OPEN = 1, A_CLOSE, A_MIN, A_MAX, A_RESTORE, A_SNAP_L, A_SNAP_R,
       A_TERM, A_APP, A_WALL, A_SORT, A_ABOUT_APP };

static int find_app(int icon){
    for(int a = 0; a < ui_app_count(); a++) if(ui_apps[a]->icon == icon) return a;
    return -1;
}

static void add(cm_item_t *m, int *n, const char *label, int action, int sep){
    cm_item_t *it = &m[(*n)++];
    ui_copy(it->label, label, sizeof it->label);
    it->action = action; it->sep = sep; it->enabled = 1; it->danger = 0;
}

/* The context packs what was clicked: an app index, plus 100 for a desktop-background click. */
static void on_right_press(rect_t bar, int n, int sw, int sh, int x, int y, u64 now){
    cm_item_t m[CM_MAX];
    int k = 0;
    start_open = 0;
    int hi = tb_hit(bar, n, x, y);
    if(hi >= 0){
        item_t it[TB_MAX_ITEMS];
        tb_items(it);
        if(it[hi].kind != IT_APP) return;
        int a = it[hi].app;
        add(m, &k, win_is_open(a) ? (win_is_min(a) ? "Show" : "Bring to front") : "Open", A_OPEN, 0);
        if(win_is_open(a)){ add(m, &k, "Close window", A_CLOSE, 1); m[k - 1].danger = 1; }
        cm_open(x, y, m, k, a, (u32)sw, (u32)sh, now);
        return;
    }
    if(isl_hit(x, y)) return;
    int a = win_at(x, y);
    if(a >= 0){
        rect_t r = win_rect_of(a);
        if(y >= (int)(r.y + UI_TITLE_H)) return;          /* inside an app: apps have no menus yet */
        win_focus(a);
        add(m, &k, "Minimize", A_MIN, 0);
        add(m, &k, win_is_max(a) ? "Restore" : "Maximize", win_is_max(a) ? A_RESTORE : A_MAX, 0);
        add(m, &k, "Snap to left half", A_SNAP_L, 1);
        add(m, &k, "Snap to right half", A_SNAP_R, 0);
        add(m, &k, "Close", A_CLOSE, 1);
        m[k - 1].danger = 1;
        cm_open(x, y, m, k, a, (u32)sw, (u32)sh, now);
        return;
    }
    int d = desk_hit(x, y);
    if(d >= 0){
        desk_select(d);
        add(m, &k, "Open", A_OPEN, 0);
        cm_open(x, y, m, k, d, (u32)sw, (u32)sh, now);
        return;
    }
    desk_select(-1);
    int files = find_app(UI_ICO_FILES), bro = find_app(UI_ICO_BROWSER), set = find_app(UI_ICO_SETTINGS);
    add(m, &k, "Open Terminal", A_TERM, 0);
    if(files >= 0){ add(m, &k, "Open Files", A_APP, 0); m[k - 1].action = A_APP + 100 * (files + 1); }
    if(bro >= 0){ add(m, &k, "Open Browser", A_APP, 0); m[k - 1].action = A_APP + 100 * (bro + 1); }
    add(m, &k, "Next wallpaper", A_WALL, 1);
    if(set >= 0){ add(m, &k, "Settings", A_APP, 0); m[k - 1].action = A_APP + 100 * (set + 1); }
    add(m, &k, "Sort icons", A_SORT, 1);
    add(m, &k, "About am-droid", A_ABOUT_APP, 0);
    cm_open(x, y, m, k, -1, (u32)sw, (u32)sh, now);
}

static void do_action(int action, int a, u64 now, int *next_vt){
    if(action >= 100){                     /* open an app named in the action */
        launch(action / 100 - 1, now, next_vt);
        return;
    }
    switch(action){
        case A_OPEN:    launch(a, now, next_vt); break;
        case A_CLOSE:   if(win_is_open(a)) win_close_app(a); break;
        case A_MIN:     win_minimize(a); break;
        case A_MAX:     if(!win_is_max(a)) win_toggle_max(a); break;
        case A_RESTORE: if(win_is_max(a)) win_toggle_max(a); break;
        case A_SNAP_L:  win_snap(a, 1); break;
        case A_SNAP_R:  win_snap(a, 2); break;
        case A_TERM:    launch(UI_MAX_APPS, now, next_vt); break;
        case A_WALL:{
            int w = (ui_wall_index() + 1) % ui_wall_count();
            ui_wall_set(w);
            ui_notify(find_app(UI_ICO_SETTINGS), "Wallpaper changed", ui_wall_name(w));
            break;
        }
        case A_SORT:    desk_sort(); break;
        case A_ABOUT_APP:{ int ab = find_app(UI_ICO_ABOUT); if(ab >= 0) launch(ab, now, next_vt); break; }
    }
}

/* A mouse press at (x, y). */
static void on_press(rect_t bar, int n, int sw, int x, int y, u64 now, int *next_vt){
    if(cm_is_open()){
        int action, ctx;
        if(cm_click(x, y, &action, &ctx)) do_action(action, ctx, now, next_vt);
        return;                                    /* a click outside only closes the menu */
    }
    if(isl_hit(x, y)){
        int a = isl_click();
        if(a >= 0) launch(a, now, next_vt);
        return;
    }

    if(start_open){
        rect_t m = tb_menu(bar, (u32)sw);
        if(ui_contains(m, x, y)){
            int a = tb_menu_hit(m, x, y);
            if(a >= 0) launch(a, now, next_vt);
            return;
        }
        start_open = 0;
    }

    int hi = tb_hit(bar, n, x, y);
    if(hi >= 0){
        item_t it[TB_MAX_ITEMS];
        tb_items(it);
        run_item(it[hi], now, next_vt);
        return;
    }

    int a = win_at(x, y);
    if(a < 0){
        /* the desktop: icons */
        int open = desk_press(x, y, now);
        if(open >= 0) launch(open, now, next_vt);
        return;
    }
    desk_select(-1);
    rect_t r = win_rect_of(a);
    if(ui_contains(win_close_button(a), x, y)){ win_close_app(a); return; }
    if(ui_contains(win_max_button(a), x, y)){ win_focus(a); win_toggle_max(a); return; }
    if(ui_contains(win_min_button(a), x, y)){ win_minimize(a); return; }
    win_focus(a);
    if(ui_contains(win_grip(a), x, y)){          /* resize corner */
        rs_app = a;
        rs_gx = (int)(r.x + r.w) - x;
        rs_gy = (int)(r.y + r.h) - y;
        return;
    }
    if(y < (int)(r.y + UI_TITLE_H)){             /* title bar: drag, or double-click to maximize */
        if(a == last_title_app && now - last_title_at < 40){
            win_toggle_max(a);
            last_title_app = -1;
            return;
        }
        last_title_app = a;
        last_title_at = now;
        drag_px = x; drag_py = y;
        win_drag_app = a;
        win_drag_dx = x - (int)r.x;
        win_drag_dy = y - (int)r.y;
        return;
    }
    const app_t *app = ui_apps[a];
    if(app->click){ ui_app_enter(a); app->click(win_body(a), x, y); ui_app_leave(); }
    win_mark_dirty(a);
}

/* Draws the desktop, back to front, inside one damaged box (the clip is set to it). */
static void compose(box_t box, u64 now, const item_t *it, int n, rect_t bar, int hover, int hov_amt, int mx, int my, u32 sw){
    /* a window that covers the whole box hides the wallpaper, the icons and the windows under it */
    int from = win_cover(box.x0, box.y0, box.x1, box.y1, now);
    if(from < 0){
        wall_draw();
        desk_draw();
    }
    win_draw_all(now, from < 0 ? 0 : from);
    isl_draw(now);
    tb_draw(it, n, bar, hover, hov_amt, win_top_app(), start_open);
    if(start_open) tb_menu_draw(bar, sw, tb_menu_slide(now, start_opened_at), mx, my);
    cm_draw(mx, my, now);
    ui_cursor(mx, my);
}

/* A key press: the Start menu's search, then the focused app, then the desktop's own keys. */
static void on_key(int c, u64 now, int *next_vt){
    int top = win_top_app();
    if(c == 27 && cm_is_open()){ cm_close(); return; }
    if(start_open){
        int r = tb_menu_key(c);
        if(r >= 0){ launch(r, now, next_vt); return; }
        if(r == TB_KEY_CLOSE){ start_open = 0; return; }
        if(r == TB_KEY_USED) return;
    }
    /* the focused app gets the key first, so typing in Notes or Calculator
     * never triggers a shortcut */
    if(top >= 0 && ui_apps[top]->key){
        ui_app_enter(top);
        int used = ui_apps[top]->key(c);
        ui_app_leave();
        win_mark_dirty(top);
        if(used) return;
    }
    if(c == 27 && top >= 0) win_close_app(top);
    else if((c == 'q' || c == 'Q') && top < 0) *next_vt = 2;      /* with no window open: to the text shell */
    else if(c == '\t') win_cycle_focus();
    else if(c >= '1' && c <= '9' && c - '1' < ui_app_count()) launch(c - '1', now, next_vt);
    else if(c == '0') launch(UI_MAX_APPS, now, next_vt);            /* Terminal */
}

int gui_run(void){
    if(!fb_ready()){
        console_puts("No framebuffer available; GUI requires GRUB framebuffer.\n");
        return 2;
    }
    mouse_init();

    u32 sw = fb_width(), sh = fb_height();
    scr_w = (int)sw; scr_h = (int)sh;
    wall_init(sw, sh);
    win_init(sw, sh);
    desk_init(sw, sh);
    gui_damage_all();

    /* first notifications: who signed in, and the network */
    static int greeted_uid = -2;
    if(greeted_uid != auth_uid()){
        greeted_uid = auth_uid();
        char body[64] = "Signed in as ";
        ui_cat(body, auth_name()[0] ? auth_name() : "nobody");
        ui_notify(-1, "Welcome to am-droid", body);
        if(net_up()){
            u32 ip = 0, gw = 0; u8 mac[6];
            net_info(&ip, &gw, mac);
            char a[24], b2[64] = "Address ";
            ui_ip(a, ip);
            ui_cat(b2, a);
            ui_notify(find_app(UI_ICO_NET), "Network connected", b2);
        }
    }

    int mx = (int)sw / 2, my = (int)sh / 2;
    int held = 0, next_vt = 0;
    int apps = ui_app_count();
    int cur_x = -1, cur_y = -1;              /* where the pointer was last drawn */
    int move_app = -1, move_x = -1, move_y = -1;

    while(!next_vt){
        u64 now = timer_ticks();
        item_t it[TB_MAX_ITEMS];
        int n = tb_items(it);
        rect_t bar = tb_bar(n, sw, sh);

        mouse_event_t ev;
        if(mouse_poll(&ev)){
            mx = mx + ev.dx < 0 ? 0 : (mx + ev.dx >= (int)sw ? (int)sw - 1 : mx + ev.dx);
            my = my + ev.dy < 0 ? 0 : (my + ev.dy >= (int)sh ? (int)sh - 1 : my + ev.dy);
            if(ev.pressed & 1) on_press(bar, n, (int)sw, mx, my, now, &next_vt);
            if(ev.pressed & 2) on_right_press(bar, n, (int)sw, (int)sh, mx, my, now);
            held = ev.btns;
            if(ev.wheel && !cm_is_open() && !start_open){
                int a = win_at(mx, my);
                if(a >= 0 && ui_apps[a]->scroll){
                    ui_app_enter(a);
                    ui_apps[a]->scroll(win_body(a), mx, my, ev.wheel);
                    ui_app_leave();
                    win_mark_dirty(a);
                }
            }
        }
        desk_update(mx, my, held & 1);
        for(int a = 0; a < apps; a++)
            if(win_is_open(a) && ui_apps[a]->tick){ ui_app_enter(a); ui_apps[a]->tick(); ui_app_leave(); }
        if(rs_app >= 0){
            if(held & 1){
                rect_t r = win_rect_of(rs_app);
                win_set_size(rs_app, mx + rs_gx - (int)r.x, my + rs_gy - (int)r.y);
            } else rs_app = -1;
        }
        if(win_drag_app >= 0){
            if(held & 1){
                /* a maximized or snapped window keeps its place until it is really moved */
                if(win_is_max(win_drag_app)){
                    int moved = (mx > drag_px ? mx - drag_px : drag_px - mx) + (my > drag_py ? my - drag_py : drag_py - my);
                    if(moved > 8) win_drag_dx = win_unsnap_for_drag(win_drag_app, mx);
                }
                /* the screen edges: left and right halves, and the top to maximize */
                win_snap_zone = mx <= 2 ? 1 : (mx >= (int)sw - 3 ? 2 : (my <= 2 ? 3 : 0));
                rect_t r = win_rect_of(win_drag_app);
                int nx = mx - win_drag_dx, ny = my - win_drag_dy;
                if(nx < 0) nx = 0;
                if(nx > (int)sw - (int)r.w) nx = (int)sw - (int)r.w;
                if(ny < 0) ny = 0;            /* windows may go up to the top edge (under the island) */
                if(ny > (int)(sh - 48 - 12) - 40) ny = (int)(sh - 48 - 12) - 40;
                if(!win_is_max(win_drag_app)) win_move(win_drag_app, nx, ny);
            } else {
                if(win_snap_zone) win_snap(win_drag_app, win_snap_zone);
                win_snap_zone = 0;
                win_drag_app = -1;
            }
        }

        /* what the pointer is over, topmost first: a menu, the island, the taskbar, a window, the desktop */
        int over_menu = cm_inside(mx, my) || (start_open && ui_contains(tb_menu(bar, sw), mx, my));
        int hover = over_menu ? -1 : tb_hit(bar, n, mx, my);
        int over_top = over_menu || hover >= 0 || isl_hit(mx, my);
        int over_win = over_top ? -1 : win_at(mx, my);
        win_set_pointer(over_top ? -1 : mx, over_top ? -1 : my);
        desk_set_hover(over_top || over_win >= 0 ? -1 : desk_hit(mx, my));
        {
            /* the focused app follows the pointer while it is over its window */
            int tp = win_top_app();
            int px = over_win == tp ? mx : -1, py = over_win == tp ? my : -1;
            if(tp >= 0 && ui_apps[tp]->move && (tp != move_app || px != move_x || py != move_y)){
                ui_app_enter(tp); ui_apps[tp]->move(win_body(tp), px, py); ui_app_leave();
            }
            move_app = tp; move_x = px; move_y = py;
        }
        if(hover != hov_item){ hov_item = hover; hov_since = now; }
        int hov_amt = !ui_anim_on || now - hov_since >= 10 ? 255 : (int)((now - hov_since) * 255 / 10);

        int rv = keyboard_get_requested_vt();
        if(rv == 1) gui_damage_all();                 /* Ctrl+Alt+F1 while here: paint everything again */
        else if(rv != -1) next_vt = rv;
        for(int k = 0; k < 32 && !next_vt && keyboard_has_input(); k++)
            on_key(keyboard_getchar(), now, &next_vt);
        if(serial_has_input()){
            int c = serial_getc();
            if(c == 'q' || c == 'Q') next_vt = 2;
        }
        if(next_vt) break;

        /* every layer reports what changed since it was last drawn */
        isl_update(sw, now, ui_anim_on);
        desk_damage(mx, my);
        win_damage(now);
        isl_damage(now);
        tb_damage(n, bar, hover, hov_amt, win_top_app(), start_open, now);
        tb_menu_damage(bar, sw, start_open, tb_menu_slide(now, start_opened_at), mx, my);
        cm_damage(mx, my, now);
        if(mx != cur_x || my != cur_y){
            gui_damage(cur_x, cur_y, UI_CURSOR_W, UI_CURSOR_H);
            gui_damage(mx, my, UI_CURSOR_W, UI_CURSOR_H);
            cur_x = mx; cur_y = my;
        }

        if(ndmg){
            box_t todo[DMG_MAX];
            int ntodo = ndmg;
            u64 t0 = sched_tsc(), pixels = 0;
            for(int i = 0; i < ndmg; i++) todo[i] = dmg[i];
            ndmg = 0;                                 /* drawing may report damage for the next frame */
            for(int i = 0; i < ntodo; i++){
                box_t b = todo[i];
                fb_set_base_clip((u32)b.x0, (u32)b.y0, (u32)(b.x1 - b.x0), (u32)(b.y1 - b.y0));
                compose(b, now, it, n, bar, hover, hov_amt, mx, my, sw);
                pixels += (u64)box_area(b);
            }
            fb_clear_base_clip();
            for(int i = 0; i < ntodo; i++)
                fb_present((u32)todo[i].x0, (u32)todo[i].y0, (u32)(todo[i].x1 - todo[i].x0), (u32)(todo[i].y1 - todo[i].y0));
            perf_frame(sched_tsc() - t0, pixels);
        }
        perf_loop(now);
        proc_sleep_ticks(1);              /* to the next timer tick; programs started from the GUI run meanwhile */
    }

    if(next_vt == 3) close_all();
    mouse_disable();
    fb_clear(0x000000);
    fb_swap();
    return next_vt;
}
