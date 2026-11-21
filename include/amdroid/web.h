#ifndef AMDROID_WEB_H
#define AMDROID_WEB_H
/* A small HTML and CSS engine: parses a page, applies its CSS, and lays it out as
 * drawing commands. No JavaScript, images or HTTPS. */
#include "amdroid/types.h"

#define WEB_CMD_TEXT 0        /* a word, at (x, y), scale, colour */
#define WEB_CMD_RECT 1        /* a filled box */
#define WEB_CMD_IMAGE 2       /* an <img>, decoded; at (x, y), scaled to (w, h) */
#define WEB_CMD_VIDEO 3       /* a <video>'s box: painted black; the browser draws the picture there */
#define WEB_CMD_BGIMG 4       /* a CSS background picture: clipped to (x, y, w, h), drawn scale x bold px big, repeated (italic bits: 1 across, 2 down), its corner at (ox, oy) from the clip's corner */

typedef struct web_cmd {
    int kind;
    int x, y, w, h;
    u32 color;
    int scale;                /* text: glyph size in quarter steps: a glyph is 2*scale x 4*scale px */
    int bold;                 /* text: draw heavier */
    int italic;               /* text: slant the glyphs */
    const char *text;         /* text commands */
    int link;                 /* index into the link table, or -1 */
    const void *dom;          /* the element this text belongs to (for clicks) */
    int img;                  /* WEB_CMD_IMAGE / BGIMG: index into the image table (web_image_pixels) */
    int ox, oy;               /* WEB_CMD_BGIMG: where the first copy starts, relative to (x, y) */
} web_cmd_t;

/* Linked stylesheets (<link rel=stylesheet href=...>): how many, and each href as written.
   The browser fetches them and hands the text to web_add_css; lay out again afterwards. */
int web_stylesheet_count(void);
const char *web_stylesheet(int i);
void web_add_css(const char *css, u32 len);                /* the next sheet, in order */
void web_add_css_at(int idx, const char *css, u32 len);    /* sheet idx (they may arrive in any order) */
/* The size of the window the page is shown in, in pixels (call before web_load: scripts and
   @media rules see it before the first layout). */
void web_set_viewport(int w, int h);
/* 1: the page's ad boxes are hidden (a built-in stylesheet). Call before web_load. */
void web_set_adblock(int on);
/* The page's address, for window.location (call before web_load). */
void web_set_url(const char *url);
/* Parses an HTML document (replaces the previous one). 0 on success. */
int web_load(const char *html, u32 len);
/* Lays the page out for a given width in pixels. Returns the total height. */
int web_layout(int width);
/* The drawing commands of the last layout, and how many. */
const web_cmd_t *web_cmds(int *n);
/* Images: the browser fetches each one (resolve web_image(i) against the page URL),
   hands the bytes to web_add_image, and lays the page out again (laid_width = -1). */
int web_image_count(void);
const char *web_image(int i);
int web_image_state(int i);         /* 0 pending, 1 ready, 2 failed */
int web_image_wanted(int i);        /* 1 once a layout showed it: only those are worth fetching */
int web_add_image(int idx, const char *data, u32 len);
int web_add_image_data_url(int idx, const char *url);   /* data:[mediatype][;base64],... */
const u32 *web_image_pixels(int i, u32 *w, u32 *h);
/* The size the page sees (SVGs are drawn larger than this, for sharpness): 0 if not ready. */
int web_image_natural(int i, u32 *w, u32 *h);
/* The page's canvas colour (<html> or else <body> background): 1 and *color, or 0. Browsers
   paint it over the whole viewport, not just behind the content. */
int web_canvas_bg(u32 *color);
/* The page title (from <title>), or "". */
const char *web_title(void);
/* The href of a link (as written in the page; relative links are not resolved), or NULL. */
const char *web_link(int idx);
/* Frees the page. */
void web_free(void);
/* Frees the page, the layout buffers and the script heap (when the browser closes). */
void web_release(void);
/* External scripts (<script src>): the browser fetches each web_script_src(i) that is not
   NULL (resolve it against the page URL), hands the text to web_add_script (len 0 when it
   failed), then calls web_run_scripts. Pages without external scripts run them in web_load. */
int web_script_count(void);
const char *web_script_src(int i);
void web_add_script(int i, const char *text, u32 len);
int web_scripts_pending(void);
int web_run_scripts(void);                 /* 1 if held-back scripts ran now */
const char *web_script_error(int i);      /* the error script i stopped with, or "" */
/* Scripts: they run when the page loads. web_js_error() is their error, or "". */
const char *web_js_error(void);
/* A click on an element: runs its JavaScript click handlers (and its ancestors').
   Returns how many ran. */
int web_click(const void *node);
/* 1 when a script changed the page and it must be laid out again. */
int web_dirty(void);
void web_clear_dirty(void);
/* Runs the timers that are due (setTimeout, setInterval). 1 if the page changed. */
int web_tick(void);
/* Dispatches an event at an element and up through its ancestors. type is "click",
   "mouseover", "mouseout", "keydown", "input"... key is the key for key events (or NULL).
   Returns how many handlers ran. */
int web_dispatch(const void *node, const char *type, const char *key);
/* Typing into a text box (<input>): returns 1 if the box took the key. */
int web_key_input(const void *node, int c);
/* A click that did not navigate: buttons and menus open (their :hover / :focus-within rules
   apply until the next click), a <summary> opens its <details>, and a search button that
   needs scripts reveals the search box next to it. Returns the text box to focus, or NULL. */
const void *web_activate(const void *node);
/* 1 if the node is a text box. */
int web_is_input(const void *node);
/* 1 if a click handler called preventDefault since the last call (clears it). */
int web_take_prevented(void);
/* A navigation a script asked for (location.href = ...), once; NULL if none. */
const char *web_take_navigation(void);
/* 1 when the page is shown by the browser's YouTube reader (see jsdom.c). */
int web_youtube_reader(void);
/* The last console.log line of the scripts, or "". */
const char *web_console(void);

/* ---- fetch() and media (media_api.inc): Husser passes these to the browser ---- */
/* the requests not answered yet (up to max), oldest first; *changed: the list changed */
typedef struct { u32 id; const char *url, *method, *ctype; const u8 *body; u32 blen; } web_fetch_req_t;
int  web_fetch_requests(web_fetch_req_t *out, int max, int *changed);
void web_set_cookies(const char *s);           /* the page's cookies ("a=b; c=d"), before its scripts run */
int  web_cookie_writes(char *out, u32 cap);    /* document.cookie assignments since the last call, one a line */
void web_fetch_done(u32 id, u32 status, const u8 *data, u32 len, int failed);
/* the next media command for the browser (see M_CMD_* in husser_proto.h) */
int  web_media_next(u32 *cmd, i32 *a, i64 *t0, i64 *t1, const u8 **data, u32 *len);
void web_media_pop(void);               /* it was handed over */
void web_media_acked(u32 state_seq);     /* the browser took it */
typedef struct web_mstate {
    u32 seq;
    i64 time_us, duration_us;
    u32 playing, ended, stalled, ready, video_w, video_h;
    u32 pending[2], nranges[2];
    i64 ranges[2][16];
    u32 error_seq;
    const char *error;
} web_mstate_t;
void web_media_state(const web_mstate_t *s);
/* the <video> box in the page (from the last layout): 1 if there is one */
int  web_video_box(int *x, int *y, int *w, int *h);

#endif
