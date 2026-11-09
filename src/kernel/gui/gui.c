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
#include "gui_internal.h"

static int start_open = 0;
static int rs_app = -1, rs_gx = 0, rs_gy = 0;      /* a window being resized, and the grab offset */
static int last_title_app = -1;                    /* for double-clicks on a title bar */
static u64 last_title_at = 0;
static int drag_px = 0, drag_py = 0;               /* where a title-bar drag started */
static u64 start_opened_at = 0;      /* when the Start menu opened, for its slide */
static int hov_item = -1, hov_amt = 0;

static void set_start(int open, u64 now){
    if(open && !start_open) start_opened_at = now;
    start_open = open;
}

/* Starts an app from the taskbar, the Start menu or a key. Terminal leaves the GUI. */
static void launch(int a, u64 now, int *next_vt){
    if(a == UI_MAX_APPS){ *next_vt = 2; return; }     /* Terminal */
    if(a == UI_MAX_APPS + 1){ *next_vt = 3; return; }  /* Sign out: back to the login screen */
    win_open_app(a, now);
    start_open = 0;
}

static void run_item(item_t it, u64 now, int *next_vt){
    if(it.kind == IT_START || it.kind == IT_SEARCH){ set_start(!start_open, now); return; }
    if(it.kind == IT_TERM){ launch(UI_MAX_APPS, now, next_vt); return; }
    int a = it.app;
    if(win_is_open(a) && !win_is_min(a) && win_top_app() == a){ win_minimize(a); return; }
    launch(a, now, next_vt);
}

/* ---- right-click menus ---- */

enum { A_OPEN = 1, A_PIN, A_UNPIN, A_CLOSE, A_MIN, A_MAX, A_RESTORE, A_SNAP_L, A_SNAP_R,
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

static void pin_items(cm_item_t *m, int *n, int a){
    add(m, n, tb_pinned(a) ? "Unpin from taskbar" : "Pin to taskbar", tb_pinned(a) ? A_UNPIN : A_PIN, 1);
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
        pin_items(m, &k, a);
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
        pin_items(m, &k, a);
        add(m, &k, "Close", A_CLOSE, 1);
        m[k - 1].danger = 1;
        cm_open(x, y, m, k, a, (u32)sw, (u32)sh, now);
        return;
    }
    int d = desk_hit(x, y);
    if(d >= 0){
        desk_select(d);
        add(m, &k, "Open", A_OPEN, 0);
        pin_items(m, &k, d);
        cm_open(x, y, m, k, d, (u32)sw, (u32)sh, now);
        return;
    }
    desk_select(-1);
    int files = find_app(UI_ICO_FILES), bro = find_app(UI_ICO_BROWSER), set = find_app(UI_ICO_SETTINGS);
    add(m, &k, "Open Terminal", A_TERM, 0);
    if(files >= 0){ add(m, &k, "Open Files", A_APP, 0); m[k - 1].action = A_APP + 100 * (files + 1); }
    if(bro >= 0){ add(m, &k, "Open BRO weser", A_APP, 0); m[k - 1].action = A_APP + 100 * (bro + 1); }
    add(m, &k, "Next wallpaper", A_WALL, 1);
    if(set >= 0){ add(m, &k, "Settings", A_APP, 0); m[k - 1].action = A_APP + 100 * (set + 1); }
    add(m, &k, "Sort icons", A_SORT, 1);
    add(m, &k, "About am-droid", A_ABOUT_APP, 0);
    cm_open(x, y, m, k, -1, (u32)sw, (u32)sh, now);
}

static void do_action(int action, int a, u64 now, int *next_vt){
    char msg[64];
    if(action >= 100){                     /* open an app named in the action */
        launch(action / 100 - 1, now, next_vt);
        return;
    }
    switch(action){
        case A_OPEN:    launch(a, now, next_vt); break;
        case A_PIN:
            tb_set_pinned(a, 1);
            ui_copy(msg, ui_apps[a]->name, sizeof msg); ui_cat(msg, " stays on the taskbar");
            ui_notify(a, "Pinned to the taskbar", msg);
            break;
        case A_UNPIN:
            tb_set_pinned(a, 0);
            ui_copy(msg, ui_apps[a]->name, sizeof msg); ui_cat(msg, " leaves the taskbar when closed");
            ui_notify(a, "Unpinned from the taskbar", msg);
            break;
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
}

int gui_run(void){
    if(!fb_ready()){
        console_puts("No framebuffer available; GUI requires GRUB framebuffer.\n");
        return 2;
    }
    mouse_init();

    u32 sw = fb_width(), sh = fb_height();
    wall_init(sw, sh);
    win_init(sw, sh);
    desk_init(sw, sh);
    ui_msg_set("");

    /* first notifications: who signed in, and the network */
    {
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
    int prev_btn = 0, next_vt = 0;
    int apps = ui_app_count();

    while(!next_vt){
        u64 now = timer_ticks();
        item_t it[TB_MAX_ITEMS];
        int n = tb_items(it);
        rect_t bar = tb_bar(n, sw, sh);

        int mdx, mdy, btns;
        if(mouse_poll(&mdx, &mdy, &btns)){
            mx = mx + mdx < 0 ? 0 : (mx + mdx >= (int)sw ? (int)sw - 1 : mx + mdx);
            my = my + mdy < 0 ? 0 : (my + mdy >= (int)sh ? (int)sh - 1 : my + mdy);
            if((btns & 1) && !(prev_btn & 1)) on_press(bar, n, (int)sw, mx, my, now, &next_vt);
            if((btns & 2) && !(prev_btn & 2)) on_right_press(bar, n, (int)sw, (int)sh, mx, my, now);
            prev_btn = btns;
        }
        desk_update(mx, my, prev_btn & 1);
        for(int a = 0; a < apps; a++)
            if(win_is_open(a) && ui_apps[a]->tick){ ui_app_enter(a); ui_apps[a]->tick(); ui_app_leave(); }
        if(rs_app >= 0){
            if(btns & 1){
                rect_t r = win_rect_of(rs_app);
                win_set_size(rs_app, mx + rs_gx - (int)r.x, my + rs_gy - (int)r.y);
            } else rs_app = -1;
        }
        win_set_pointer(mx, my);
        if(win_drag_app >= 0){
            if(btns & 1){
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
        int hover = tb_hit(bar, n, mx, my);
        {
            int tp = win_top_app();
            if(tp >= 0 && ui_apps[tp]->move){ ui_app_enter(tp); ui_apps[tp]->move(win_body(tp), mx, my); ui_app_leave(); }
        }
        if(hover != hov_item){ hov_item = hover; hov_amt = 0; }
        else if(hov_amt < 255) hov_amt = ui_anim_on ? (hov_amt + 51 > 255 ? 255 : hov_amt + 51) : 255;

        int rv = keyboard_get_requested_vt();
        if(rv != -1 && rv != 1) next_vt = rv;
        if(keyboard_has_input()){
            int c = keyboard_getchar();
            int top = win_top_app();
            /* the focused app gets the key first, so typing in Notes or Calculator
             * never triggers a shortcut */
            int used = 0;
            if(c == 27 && cm_is_open()){ cm_close(); used = 1; }
            else if(top >= 0 && ui_apps[top]->key){ ui_app_enter(top); used = ui_apps[top]->key(c); ui_app_leave(); }
            if(used){ }
            else if(c == 27 && start_open) start_open = 0;
            else if(c == 27 && top >= 0) win_close_app(top);
            else if(c == 'q' || c == 'Q') next_vt = 2;
            else if(c == '\t') win_cycle_focus();
            else if(c >= '1' && c <= '9' && c - '1' < apps){
                launch(c - '1', now, &next_vt);
            }
            else if(c == '0') launch(UI_MAX_APPS, now, &next_vt);     /* Terminal */
        }
        if(serial_has_input()){
            int c = serial_getc();
            if(c == 'q' || c == 'Q') next_vt = 2;
        }
        if(next_vt) break;

        isl_update(sw, now, ui_anim_on);
        wall_draw();
        desk_draw(mx, my);
        win_draw_all(now, ui_anim_on);
        isl_draw(now);
        tb_draw(it, n, bar, hover, hov_amt, win_top_app(), start_open, sw);
        if(start_open){
            int dy = 0;
            if(ui_anim_on && now - start_opened_at < 12) dy = (255 - ui_ease((int)((now - start_opened_at) * 255 / 12))) * 24 / 255;
            tb_menu_draw(bar, sw, dy, mx, my);
        }
        cm_draw(mx, my, now);
        ui_cursor(mx, my);
        fb_swap();
        proc_sleep_ticks(2);              /* ~16 ms; programs started from the GUI run meanwhile */
    }

    mouse_disable();
    fb_clear(0x000000);
    fb_swap();
    return next_vt;
}
