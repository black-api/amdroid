#ifndef AMDROID_UI_H
#define AMDROID_UI_H
/* Shared API for the GUI core and for apps (the files in the apps folder).
 * An app is one file: it fills an app_t and is listed in apps/registry.c. */
#include "amdroid/types.h"
#include "amdroid/font.h"

typedef struct { u32 x, y, w, h; } rect_t;

/* ---- palette (0x00RRGGBB) ---- */
#define C_WIN       0x1C2328      /* window surface */
#define C_WIN_EDGE  0x2F3A42
#define C_SEP       0x2A343B
#define C_BAR       0x161C21      /* taskbar pill */
#define C_BAR_EDGE  0x2B353D
#define C_MENU      0x1A2026
#define C_CARD      0x222B31
#define C_CARD_HI   0x2C373F
#define C_HOVER     0x2F3B44
#define C_SHADOW    0x030405
#define C_TEXT      0xF2F5F7
#define C_MUTED     0x9AA7B0
#define C_DIM       0x63717B
#define C_GOOD      0x3DDC97
#define C_WARN      0xFFB547

/* ---- metrics ---- */
#define UI_SCALE    2                       /* UI text is drawn at 2x */
#define UI_CW       (FONT_W * UI_SCALE)     /* 16 px per character */
#define UI_CH       (FONT_H * UI_SCALE)     /* 32 px per line */
#define UI_TITLE_H  48                      /* window title bar */
#define UI_PAD      20                      /* inside a window */
#define UI_ROW_H    40                      /* list rows */
#define UI_HEAD_Y   56                      /* top of a list header, from the body top */
#define UI_BTN_H    40                      /* buttons */

/* ---- settings the user can change (ui.c) ---- */
#define UI_NACCENT 4
int         ui_accent_index(void);
void        ui_set_accent(int i);
u32         ui_accent(void);
u32         ui_accent_color_of(int i);
const char *ui_accent_name(int i);
extern int  ui_anim_on;                     /* animations on/off */

/* ---- wallpaper settings (wallpaper.c) ---- */
int         ui_wall_count(void);
int         ui_wall_index(void);
void        ui_wall_set(int i);
const char *ui_wall_name(int i);
u32         ui_wall_swatch(int i);          /* a colour to preview it with */

struct app;
/* ---- notifications: they appear in the Dynamic Island, one after another ----
 * app is the app a click on the notification opens (an index into ui_apps), or -1.
 * ui_msg_set(s) is a short form: a notification with just a title. */
void        ui_notify(int app, const char *title, const char *body);
void        ui_msg_set(const char *s);
int         ui_app_index(const struct app *app);   /* the index of an app in ui_apps, or -1 */
/* Accounting (the GUI wraps every call into an app with these): CPU cycles used by an app's
 * code, and its heap blocks are tagged with owner app+1. */
void        ui_app_enter(int a);
void        ui_app_leave(void);
u64         ui_app_cycles(int a);
int         ui_app_is_open(int a);          /* the app has a window (maybe minimized) */
int         ui_app_is_front(int a);         /* its window is the one in front, where keys go */
void        ui_app_close(int a);            /* closes its window */

/* ---- drawing ----
 * A window is drawn again when its app gets a key, a click or the wheel. An app whose
 * picture changes at any other time (a clock, a download: in its tick or its move) says so: */
void ui_redraw(void);
void ui_fill(rect_t r, u32 c);
/* Animation helpers: t runs 0..255. ui_ease gives an ease-out curve, ui_lerp blends colours. */
int  ui_ease(int t);
u32  ui_lerp(u32 a, u32 b, int t);
void ui_rrect(u32 x, u32 y, u32 w, u32 h, u32 r, u32 c);   /* rounded rectangle */
/* Anti-aliased primitives (coordinates are pixel-centred; cx,cy is an exact centre). */
void ui_disc(int cx, int cy, int r, u32 c);                /* filled circle */
void ui_ring(int cx, int cy, int r, int thick, u32 c);     /* circle outline */
void ui_line(int x0, int y0, int x1, int y1, int thick, u32 c); /* thick line segment */
void ui_text(u32 x, u32 y, const char *s, u32 fg, u32 scale);
u32  ui_text_w(const char *s, u32 scale);
void ui_text_right(rect_t r, u32 y, const char *s, u32 fg, u32 scale);
int  ui_contains(rect_t r, int x, int y);
void ui_icon(int which, u32 x, u32 y, u32 c);               /* 32x32 icon */
void ui_cursor(int x, int y);                              /* the mouse pointer */
#define UI_CURSOR_W 12
#define UI_CURSOR_H 16

/* ---- text helpers (no libc in the kernel) ---- */
void ui_num(u32 v, char *out);
void ui_cat(char *dst, const char *src);
void ui_copy(char *dst, const char *src, u32 n);
int  ui_streq(const char *a, const char *b);
u32  ui_len(const char *s);
void ui_uptime(char *out, u64 ticks);      /* hh:mm:ss, out needs 9 bytes */
void ui_clock(char *out);                  /* hh:mm from the RTC, 6 bytes */
void ui_ip(char *out, u32 ip);

/* ---- lists inside a window body: a header row, then rows ---- */
rect_t ui_list_head(rect_t b);
rect_t ui_list_row(rect_t b, int i);
int    ui_list_rows(rect_t b);             /* how many rows fit in the body */
rect_t ui_button(rect_t b, u32 w);         /* a button at the top right of the body */

/* ---- icons ---- */
enum { UI_ICO_START, UI_ICO_SEARCH, UI_ICO_FILES, UI_ICO_TASKS,
       UI_ICO_SETTINGS, UI_ICO_ABOUT, UI_ICO_TERM,
       UI_ICO_CALC, UI_ICO_CLOCK, UI_ICO_NOTES, UI_ICO_NET, UI_ICO_BROWSER,
       UI_ICO_STORE };

/* ---- apps ----
 * open/close are optional (NULL). draw is called every frame with the
 * window body. key and click get input while the window has focus.
 * Coordinates passed to click are screen pixels. */
typedef struct app {
    const char *name;
    int icon;                                /* UI_ICO_* */
    void (*open)(void);
    void (*close)(void);
    void (*draw)(rect_t body, int focused);
    int  (*key)(int c);                      /* return 1 if used; Esc closes the window when 0 */
    void (*click)(rect_t body, int x, int y);
    void (*move)(rect_t body, int x, int y);  /* the pointer moved, for the focused window; (-1, -1) when it left */
    void (*tick)(void);                       /* every timer tick while the app is open, even minimized (optional) */
    void (*scroll)(rect_t body, int x, int y, int dz);   /* the wheel over the window: dz > 0 is down (optional) */
} app_t;

/* The apps, in the order of their keys (1, 2, 3, ...). NULL-terminated. */
#define UI_MAX_APPS 10
extern const app_t *const ui_apps[UI_MAX_APPS + 1];
int ui_app_count(void);

/* ---- how the screen is doing, over the last second (for the Tasks app) ---- */
typedef struct {
    u32 loops;           /* turns of the GUI loop: input is read this often */
    u32 redraws;         /* turns that had something to draw */
    u32 avg_us, max_us;  /* time to draw and show one of those */
    u32 screen_tenths;   /* pixels drawn, in tenths of a screen (10 = the whole screen once) */
} ui_perf_t;
void ui_perf(ui_perf_t *out);

#endif
