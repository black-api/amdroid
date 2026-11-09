#ifndef AMDROID_WEB_H
#define AMDROID_WEB_H
/* A small HTML and CSS engine: parses a page, applies its CSS, and lays it out as
 * drawing commands. No JavaScript, images or HTTPS. */
#include "amdroid/types.h"

#define WEB_CMD_TEXT 0        /* a word, at (x, y), scale, colour */
#define WEB_CMD_RECT 1        /* a filled box */
#define WEB_CMD_IMAGE 2       /* an <img>, decoded; at (x, y), scaled to (w, h) */

typedef struct web_cmd {
    int kind;
    int x, y, w, h;
    u32 color;
    int scale;                /* text: 1 = 16 px glyphs, 2 = 32 px, ... */
    int bold;                 /* text: draw heavier */
    const char *text;         /* text commands */
    int link;                 /* index into the link table, or -1 */
    const void *dom;          /* the element this text belongs to (for clicks) */
    int img;                  /* WEB_CMD_IMAGE: index into the image table (web_image_pixels) */
} web_cmd_t;

/* Linked stylesheets (<link rel=stylesheet href=...>): how many, and each href as written.
   The browser fetches them and hands the text to web_add_css; lay out again afterwards. */
int web_stylesheet_count(void);
const char *web_stylesheet(int i);
void web_add_css(const char *css, u32 len);
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
int web_add_image(int idx, const char *data, u32 len);
int web_add_image_data_url(int idx, const char *url);   /* data:[mediatype][;base64],... */
const u32 *web_image_pixels(int i, u32 *w, u32 *h);
/* The page title (from <title>), or "". */
const char *web_title(void);
/* The href of a link (as written in the page; relative links are not resolved), or NULL. */
const char *web_link(int idx);
/* Frees the page. */
void web_free(void);
/* Frees the page, the layout buffers and the script heap (when the browser closes). */
void web_release(void);
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
/* 1 if the node is a text box. */
int web_is_input(const void *node);
/* 1 if a click handler called preventDefault since the last call (clears it). */
int web_take_prevented(void);
/* The last console.log line of the scripts, or "". */
const char *web_console(void);

#endif
