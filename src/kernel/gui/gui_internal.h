#ifndef AMDROID_GUI_INTERNAL_H
#define AMDROID_GUI_INTERNAL_H
/* Shared between the GUI core modules (not used by apps). */
#include "amdroid/ui.h"

/* ---- gui.c: repainting. A layer calls these when what it shows has changed. ---- */
void gui_damage(int x, int y, int w, int h);
void gui_damage_all(void);

/* ---- wallpaper.c ---- */
void wall_init(u32 w, u32 h);
void wall_draw(void);

/* ---- windows.c ---- */
#define WIN_ANIM_TICKS 20                 /* open animation: 200 ms */
void   win_init(u32 sw, u32 sh);
int    win_is_open(int a);
int    win_is_min(int a);
rect_t win_rect_of(int a);
rect_t win_body(int a);
rect_t win_close_button(int a);
int    win_top_app(void);
int    win_at(int x, int y);             /* the topmost visible window under (x, y), or -1 */
void   win_open_app(int a, u64 now);
void   win_close_app(int a);
void   win_minimize(int a);
void   win_focus(int a);
void   win_cycle_focus(void);
void   win_move(int a, int x, int y);
void   win_set_size(int a, int w, int h);
rect_t win_grip(int a);             /* the resize corner of a window */
rect_t win_max_button(int a);
rect_t win_min_button(int a);
void   win_toggle_max(int a);
void   win_snap(int a, int zone);      /* 1 left half, 2 right half, 3 maximize */
int    win_unsnap_for_drag(int a, int mx);
int    win_is_max(int a);
void   win_set_pointer(int x, int y);   /* the pointer when it is over a window or the desktop, else -1 */
void   win_mark_dirty(int a);          /* the app's window shows something new */
void   win_damage(u64 now);            /* reports the windows that moved or changed */
extern int win_snap_zone;
#define WIN_MIN_W 360
#define WIN_MIN_H 240
void   win_draw_all(u64 now, int from);   /* from: the lowest window (by stacking) to draw */
int    win_cover(int x0, int y0, int x1, int y1, u64 now);   /* the topmost window hiding all of the box, or -1 */
extern int win_drag_app, win_drag_dx, win_drag_dy;

/* ---- taskbar.c ---- */
#define IT_START  0
#define IT_SEARCH 1
#define IT_APP    2
#define IT_TERM   3
#define TB_MAX_ITEMS (UI_MAX_APPS + 4)
typedef struct { int kind; int app; } item_t;
int    tb_items(item_t *it);
rect_t tb_bar(int n, u32 sw, u32 sh);
int    tb_hit(rect_t bar, int n, int x, int y);
void   tb_draw(const item_t *it, int n, rect_t bar, int hover, int hover_amt, int top, int start_open);
void   tb_damage(int n, rect_t bar, int hover, int hover_amt, int top, int start_open, u64 now);
/* The Start menu: a search box and the entries that match it. An entry is an app index,
 * UI_MAX_APPS for Terminal or UI_MAX_APPS + 1 for Sign out. */
#define TB_KEY_UNUSED (-1)
#define TB_KEY_USED   (-2)
#define TB_KEY_CLOSE  (-3)
rect_t tb_menu(rect_t bar, u32 sw);
void   tb_menu_reset(void);                   /* when it opens: an empty search */
const char *tb_menu_pkg(int id);              /* installed-app id for a menu id, or NULL */
void   gui_run_pkg(const char *id);           /* launch an installed .adp app full screen */
int    tb_menu_hit(rect_t m, int x, int y);   /* the entry at (x, y), or -1 */
int    tb_menu_key(int c);                    /* an entry to open, or one of TB_KEY_* */
int    tb_menu_slide(u64 now, u64 opened_at); /* how far below its place the opening menu still is */
void   tb_menu_draw(rect_t bar, u32 sw, int dy, int mx, int my);   /* dy: slide offset; mx, my: the pointer */
void   tb_menu_damage(rect_t bar, u32 sw, int open, int dy, int mx, int my);

/* ---- island.c ---- */
void isl_update(u32 sw, u64 now, int animate);
void isl_draw(u64 now);
void isl_damage(u64 now);
int  isl_hit(int x, int y);                   /* click on the island */
int  isl_click(void);                         /* returns the app a clicked notification opens, or -1 */

/* ---- desktop.c: icons on the desktop ---- */
void desk_init(u32 sw, u32 sh);
void desk_draw(void);
void desk_damage(int mx, int my);
void desk_set_hover(int a);                   /* the icon under the pointer, or -1 */
int  desk_hit(int x, int y);                  /* the app whose icon is at (x, y), or -1 */
int  desk_press(int x, int y, u64 now);       /* returns an app to open on a double-click, or -1 */
void desk_update(int x, int y, int down);     /* drags; drops on release */
void desk_select(int a);
void desk_sort(void);

/* ---- menu.c: right-click menus ---- */
#define CM_MAX 10
typedef struct { char label[32]; int action; int sep; int enabled; int danger; } cm_item_t;   /* sep: a line above it */
void cm_open(int x, int y, const cm_item_t *list, int n, int ctx, u32 sw, u32 sh, u64 now);
int  cm_is_open(void);
void cm_close(void);
int  cm_click(int x, int y, int *action, int *ctx);   /* 1 with the action when an item was chosen */
int  cm_inside(int x, int y);
void cm_draw(int mx, int my, u64 now);
void cm_damage(int mx, int my, u64 now);

#endif
