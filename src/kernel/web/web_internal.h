#ifndef AMDROID_WEB_INTERNAL_H
#define AMDROID_WEB_INTERNAL_H
#include "amdroid/web.h"

enum { W_NONE, W_BLOCK, W_INLINE, W_LISTITEM, W_FLEX };

typedef struct wstyle {
    u32 color;
    int bg_set;
    u32 bg;
    int size;                 /* font size in px; 16 px glyphs are scale 1 */
    int bold, italic, underline;
    int align;                /* 0 left, 1 centre, 2 right */
    int display;
    int m[4];                 /* top, right, bottom, left margins (px) */
    int p[4];                 /* padding, same order */
    int width, height;        /* px; 0 = automatic */
    int width_pct, width_vw;  /* width as tenths of a percent of the container / of the viewport */
    int max_w, max_pct, max_vw;   /* max-width, the same three ways (0 = none) */
    int auto_lr;              /* margin auto: 1 right, 2 left (both: centred) */
    int border_box;           /* box-sizing: border-box (width includes padding and border) */
    int border;               /* border width in px (drawn on all four sides) */
    u32 border_color;
    int gap;                  /* flex: space between items */
    int no_bullet;            /* list-style: none (inherited) */
    int hide_text;            /* font-size: 0 or text-indent far to the left: the text is not shown (inherited) */
    int zero_h, ovf_hidden;   /* height: 0 and overflow: hidden (together: nothing shows) */
    int flex_grow;            /* flex-grow (0 = keep its size) */
    int flex_wrap;            /* flex-wrap: wrap */
    int flex_col;             /* flex-direction: column */
    int justify;              /* justify-content: 0 start, 1 centre, 2 end, 3 space-between, 4 space-around */
} wstyle_t;

typedef struct wnode {
    int text;                 /* 1: text node, 0: element */
    char tag[12];
    char *txt;                /* text nodes: the raw text */
    u32 tlen;
    char *id, *cls, *href, *style, *alt, *onclick, *value;   /* alt doubles as <input placeholder> */
    struct wnode *parent, *first, *last, *next;
    wstyle_t st;
    void *jsobj;              /* the script object for this element, once made */
    int skip;                 /* <script> of a type that is not JavaScript (JSON, templates, modules) */
    int css_link;             /* <link>: 1 rel=stylesheet, 2 media=print */
    int hidden;               /* the hidden attribute */
    u64 sig;                  /* bits for its tag, id and classes (set while styling; see css.c) */
} wnode_t;

/* html.c */
extern wnode_t *web_root;
extern char *web_css;                 /* all <style> text, concatenated */
extern char *web_css_ext;             /* the linked stylesheets that have been fetched */
extern u32 web_css_ext_len;
extern u32 web_css_len;
extern char *web_js;                  /* all <script> text */
extern u32 web_js_len;
#define WEB_LINKS_MAX   2048
#define WEB_SCRIPTS_MAX 128
extern u32 web_script_start[WEB_SCRIPTS_MAX], web_script_len[WEB_SCRIPTS_MAX];
extern int web_nscripts;
extern char web_title_buf[128];
extern char *web_link_list[WEB_LINKS_MAX];
extern int web_nlinks;
void web_nodes_reset(void);
void web_pool_range(void **p, u32 *n);
wnode_t *web_new_node(void);

/* jsdom.c: the script side of the page */
void dom_reset(void);
void dom_run_scripts(void);
void dom_mark_dirty(void);

/* css.c */
void css_reset(void);
void css_add_sheet(const char *text, u32 len);
int  css_matches(const wnode_t *n, const char *sel);   /* for querySelector */
void css_set_viewport(int w);                         /* the page width @media rules see */
void css_apply(wnode_t *n, wnode_t *parent);   /* computes n->st from the parent's style */
int  css_tag_is_hidden(const char *tag);       /* head, script, style, title, meta... */

#endif
