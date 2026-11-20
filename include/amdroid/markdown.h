#ifndef AMDROID_MARKDOWN_H
#define AMDROID_MARKDOWN_H
/* Markdown for the whole OS: one parser, two outputs.
 *
 *   md_to_html  - Markdown to an HTML fragment (the browser shows .md files with md_page)
 *   md_draw     - Markdown drawn straight to the screen through two callbacks (Notes, and
 *                 .adp apps through the Fuss builtin md_draw)
 *
 * Understood: # headings (and === / --- underlines), paragraphs, **bold**, *italic*,
 * ~~strike~~, `code`, fenced and indented code blocks, > quotes (nested), - * + and 1. lists
 * (nested, with [ ] / [x] tasks), --- rules, | tables |, [links](url), ![images](url),
 * <autolinks> and bare http(s) links, backslash escapes, hard line breaks, raw HTML blocks. */
#include "amdroid/types.h"

/* HTML for md (an allocated, 0-terminated string the caller kfree's; NULL when out of memory). */
char *md_to_html(const char *md, u32 len, u32 *out_len);
/* A whole page: title, a readable stylesheet, and md_to_html of md. */
char *md_page(const char *md, u32 len, const char *title, u32 *out_len);
/* 1 when a path or URL names a Markdown file (.md, .markdown, .mdown). */
int md_is_markdown_path(const char *path);

/* Drawing: the caller provides the two primitives and the colours. Coordinates are pixels;
 * a text callback gets a glyph scale (8*scale wide, 16*scale tall). Nothing outside
 * [clip_top, clip_bottom) is drawn (but it is still laid out, so the height is right). */
typedef struct md_draw {
    void *ctx;
    void (*text)(void *ctx, int x, int y, const char *s, u32 color, int scale, int bold);
    void (*rect)(void *ctx, int x, int y, int w, int h, u32 color);
    int scale;                 /* body text scale (1 or 2) */
    u32 fg, dim, accent, code_fg, code_bg, rule, quote;
    int clip_top, clip_bottom;
} md_draw_t;
/* Lays md out in a column w pixels wide from (x, y) and draws it. Returns the height used. */
int md_draw(const char *md, u32 len, int x, int y, int w, md_draw_t *d);

#endif
