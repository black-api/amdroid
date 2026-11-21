#ifndef AMDROID_WEB_INTERNAL_H
#define AMDROID_WEB_INTERNAL_H
#include "amdroid/web.h"

enum { W_NONE, W_BLOCK, W_INLINE, W_LISTITEM, W_FLEX };

/* Device pixels per CSS pixel: 3/2. Glyphs are drawn at quarter-step sizes, half as wide as
 * they are tall, which is about how wide real fonts run: so 16 px body text is 12 x 24 glyphs,
 * and every CSS length, image and @media width is scaled the same way, which keeps text and
 * the boxes a page sizes for it in proportion. A full-HD window is ~1280 CSS px wide: pages
 * use their desktop layouts. */
#define WEB_DPR_NUM 3
#define WEB_DPR_DEN 2
#define CSSPX(v)    ((v) * WEB_DPR_NUM / WEB_DPR_DEN)
#define GTR_PCT     100000000

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
    int border;               /* border width in px, when the same on all four sides (kept for metrics) */
    u32 border_color;
    int bt, br, bb, bl;       /* per-side border widths (0 = no border on that side) */
    int gap;                  /* flex: space between items */
    int no_bullet;            /* list-style: none (inherited) */
    int white_pre;            /* white-space: pre — keep newlines and runs of spaces (inherited) */
    int hide_text;            /* font-size: 0 or text-indent far to the left: the text is not shown (inherited) */
    int zero_h, ovf_hidden;   /* height: 0 and overflow: hidden (together: nothing shows) */
    int flex_grow;            /* flex-grow (0 = keep its size) */
    int flex_wrap;            /* flex-wrap: wrap */
    int flex_col;             /* flex-direction: column */
    int justify;              /* justify-content: 0 start, 1 centre, 2 end, 3 space-between, 4 space-around */
    u32 under;                /* the colour behind this box (the nearest ancestor background), for alpha */
    int rgap;                 /* flex/grid: space between lines (row-gap) */
    int basis, basis_pct;     /* flex-basis: px, or tenths of a percent */
    u8 basis_set;
    u8 contents;              /* display: contents: no box, its children take its place */             /* flex-basis given (it then wins over width, even when 0) */
    int min_w;                /* min-width (px) */
    short grid;               /* display: grid */
    short grid_cols;          /* grid-template-columns: how many tracks (0 = one column) */
    int grid_min;             /* repeat(auto-fill, minmax(X, 1fr)): X in px (columns are worked out) */
    int gtr[16];              /* the column tracks: > 0 px, < 0 -fr (tenths), 0 auto, GTR_PCT + tenths of a % */
    const char *gareas;       /* grid-template-areas, as written ("'a b' 'c d'") */
    const char *garea;        /* grid-area: the area's name */
    short gcs, gce, grs, gre; /* grid-column / grid-row start and end lines (0 auto, < 0 from the end) */
    short gcspan, grspan;     /* ... or spans (0 = 1) */
    u8 inline_block;          /* display: inline-block (and inline-flex/inline-table) */
    u8 float_;                /* 1 left, 2 right */
    u8 clear;                 /* clear: 1 left, 2 right, 3 both */
    u8 pos;                   /* 0 static, 1 relative, 2 absolute, 3 fixed, 4 sticky */
    u8 upper;                 /* text-transform: 1 uppercase, 2 lowercase (inherited) */
    u8 trow, tcell;           /* display: table-row / table-cell */
    u8 opacity0, clipped, offscreen, scale0;   /* ways a page hides an element */
    u8 bg_img;                /* a background image (url, gradient...) was given */
    u8 bg_stand;              /* bg_set is only a dark stand-in for a picture (light text on a hero image) */
    u8 color_clear;           /* the text colour is transparent: the text is only there for screen readers */
    u8 bg_repeat;             /* background-repeat: bit 0 across, bit 1 down (default both) */
    u8 bg_size_mode;          /* 0 auto, 1 cover, 2 contain, 3 lengths */
    int bg_idx;               /* the image table entry of background-image: url(...), plus one (0 = none) */
    int bg_w, bg_h;           /* background-size lengths (px; -1 = auto) */
    short bg_wp, bg_hp;       /* ... and tenths of a percent of the box */
    int bg_x, bg_y;           /* background-position (px) */
    short bg_xp, bg_yp;       /* ... and tenths of a percent (of the room left around the picture) */
    u8 no_shrink;             /* flex-shrink: 0 */
    u8 max_h0;                /* max-height: 0 */
    u8 strike;                /* text-decoration: line-through */
    u8 masked;                /* mask-image: the background is an icon's colour, not a box */
    u8 off_set;               /* which of top, right, bottom, left are given (bits 1, 2, 4, 8) */
    int off[4];               /* top, right, bottom, left (px) */
    short off_pct[4];         /* ... or tenths of a percent of the containing block */
} wstyle_t;

typedef struct wattr { char *name, *val; struct wattr *next; } wattr_t;

typedef struct wnode {
    int text;                 /* 1: text node, 0: element */
    char tag[12];
    char *txt;                /* text nodes: the raw text */
    u32 tlen;
    char *id, *cls, *href, *style, *alt, *onclick, *value;   /* alt doubles as <input placeholder> */
    wattr_t *attrs;           /* any other attributes (data-*, type, name, ...) */
    struct wnode *parent, *first, *last, *next;
    wstyle_t st;
    void *jsobj;              /* the script object for this element, once made */
    int skip;                 /* <script> of a type that is not JavaScript (JSON, templates, modules) */
    int css_link;             /* <link>: 1 rel=stylesheet, 2 media=print */
    int hidden;               /* the hidden attribute */
    int img;                  /* <img>: index into the image table + 1 (0 = none / not an <img>) */
    u64 sig;                  /* bits for its tag, id and classes (set while styling; see css.c) */
    u16 eidx, ecnt;           /* position among its element siblings (1-based) and how many (styling) */
    u16 tidx, tcnt;           /* the same among siblings of its own tag */
    u8 force_show;            /* shown even if the page hides it (a search box opened by a click) */
    struct wnode *pe[2];      /* the ::before and ::after boxes made for it (css_pseudo) */
    u8 pseudo;                /* this node is one: 1 ::before, 2 ::after */
    u32 cs_gen;               /* layout: content_size() is cached for this layout pass */
    int cs_max, cs_min;
} wnode_t;

/* html.c */
extern wnode_t *web_root;
extern char *web_css;                 /* all <style> text, concatenated */
extern char *web_css_ext;             /* the linked stylesheets that have been fetched */
extern u32 web_css_ext_len;
extern u32 web_css_len;
extern u32 web_css_gen;               /* bumped when the page (or its <style> text) changes */
extern u32 web_sheets_gen;            /* bumped when a linked stylesheet arrives */
const char *web_sheet_text(int i, u32 *len);
int web_sheet_slots(void);
void img_mark_wanted(int idx);
extern wnode_t *web_active;           /* the element the user clicked open: :hover/:focus-within hold for it */        /* the layout reached this image: fetch it */
extern int web_quirks;                /* no <!DOCTYPE html>: old-page rendering rules */
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
void web_pool_roots(void);            /* registers the node pool with the script collector */
wnode_t *web_new_node(void);
/* Images: registered at parse time (index+1 stored in the node), decoded as they arrive. */
int img_register(const char *url);
int css_parse_color(const char *lower, u32 n, u32 *rgb);   /* 1 colour, 2 none, 3 currentColor, 0 not a colour */
int svg_decode(const u8 *data, u32 len, u32 **out_px, u32 *out_w, u32 *out_h, int *ss);
void web_images_reset(void);
/* Generic attribute store for names without a dedicated field. */
void wnode_attr_set(wnode_t *n, const char *name, const char *val, u32 vlen);
const char *wnode_attr_get(wnode_t *n, const char *name);
/* Parses an HTML fragment, appending nodes under parent (element.innerHTML). */
void web_parse_fragment(const char *html, u32 len, wnode_t *parent);

/* jsdom.c: the script side of the page */
void dom_reset(void);
void dom_run_scripts(void);
void dom_mark_dirty(void);

/* css.c */
void css_reset(void);
void css_add_sheet(const char *text, u32 len);
void css_prepare(int width);              /* before styling: parse changed sheets, evaluate @media */
int  css_var_mark(void);                  /* custom properties in scope while styling a subtree */
void css_var_pop(int mark);
void css_number_children(wnode_t *parent); /* sibling positions for :nth-child and friends */
int  css_media_matches(const char *query);  /* window.matchMedia */
int  css_viewport_w(void);                  /* the window in CSS px */
int  css_viewport_h(void);
int  css_matches(const wnode_t *n, const char *sel);   /* for querySelector */
void css_set_viewport(int w);                         /* the page width @media rules see */
void css_apply(wnode_t *n, wnode_t *parent);   /* computes n->st from the parent's style */
int  css_pseudo(wnode_t *n);                   /* right after css_apply(n): its ::before/::after boxes */
int  css_tag_is_hidden(const char *tag);       /* head, script, style, title, meta... */

#endif
