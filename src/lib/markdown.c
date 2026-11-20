/* markdown.c - see include/amdroid/markdown.h.
 *
 * The block parser walks the text line by line and reports blocks (heading, paragraph, code,
 * quote, list item, table, rule, html) to an output; the inline parser turns a block's text
 * into runs (plain, bold, italic, code, strike, link, image). Two outputs share them: an HTML
 * writer and a drawing layout. No libraries: kmalloc and a few string helpers only. */
#include "amdroid/markdown.h"
#include "amdroid/heap.h"
#include "string.h"

/* ================= helpers ================= */

static int is_sp(char c){ return c == ' ' || c == '\t'; }
static int is_digit(char c){ return c >= '0' && c <= '9'; }
static int is_alnum(char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c); }
static int is_punct(char c){ return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'); }
static int lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int md_is_markdown_path(const char *p){
    if(!p) return 0;
    u32 n = 0;
    while(p[n] && p[n] != '?' && p[n] != '#') n++;
    static const char *ext[] = { ".md", ".markdown", ".mdown", ".mkd", 0 };
    for(int i = 0; ext[i]; i++){
        u32 el = (u32)strlen(ext[i]);
        if(n < el) continue;
        u32 k = 0;
        while(k < el && lower(p[n - el + k]) == ext[i][k]) k++;
        if(k == el) return 1;
    }
    return 0;
}

/* a growable output buffer */
typedef struct { char *b; u32 n, cap; int oom; } buf_t;
static void put_n(buf_t *o, const char *s, u32 n){
    if(o->oom) return;
    if(o->n + n + 1 > o->cap){
        u32 nc = o->cap ? o->cap * 2 : 4096;
        while(nc < o->n + n + 1) nc *= 2;
        char *g = kmalloc(nc);
        if(!g){ o->oom = 1; return; }
        if(o->b){ memcpy(g, o->b, o->n); kfree(o->b); }
        o->b = g; o->cap = nc;
    }
    memcpy(o->b + o->n, s, n);
    o->n += n;
    o->b[o->n] = 0;
}
static void put(buf_t *o, const char *s){ put_n(o, s, (u32)strlen(s)); }
static void put_esc(buf_t *o, const char *s, u32 n){
    for(u32 i = 0; i < n; i++){
        char c = s[i];
        if(c == '<') put(o, "&lt;");
        else if(c == '>') put(o, "&gt;");
        else if(c == '&') put(o, "&amp;");
        else if(c == '"') put(o, "&quot;");
        else put_n(o, &c, 1);
    }
}
static void put_num(buf_t *o, int v){
    char t[12]; int k = 0;
    if(v < 0){ put(o, "-"); v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while(v && k < 11);
    while(k) put_n(o, &t[--k], 1);
}

/* ================= inline runs ================= */

enum { MS_B = 1, MS_I = 2, MS_CODE = 4, MS_S = 8, MS_LINK = 16, MS_IMG = 32, MS_BR = 64 };
typedef struct { u8 st; const char *t; u32 n; const char *href; u32 hn; } run_t;
typedef struct { run_t *r; int n, cap; } runs_t;

static void add_run(runs_t *rs, u8 st, const char *t, u32 n, const char *href, u32 hn){
    if(!n && !(st & (MS_BR | MS_IMG))) return;
    if(rs->n >= rs->cap) return;
    /* plain text right after plain text of the same kind joins it */
    if(rs->n && !(st & (MS_BR | MS_IMG | MS_CODE))){
        run_t *p = &rs->r[rs->n - 1];
        if(p->st == st && p->t + p->n == t && p->href == href) { p->n += n; return; }
    }
    rs->r[rs->n++] = (run_t){ st, t, n, href, hn };
}

/* Finds a closing delimiter run of exactly k chars c at or after i (not preceded by a space). */
static u32 find_close(const char *s, u32 n, u32 i, char c, u32 k){
    for(u32 j = i; j + k <= n; j++){
        if(s[j] == '\\'){ j++; continue; }
        if(s[j] == '`'){ u32 e = j + 1; while(e < n && s[e] != '`') e++; j = e; continue; }
        u32 m = 0;
        while(j + m < n && s[j + m] == c) m++;
        if(m == k && j > i && !is_sp(s[j - 1])){
            if(c == '_' && j + m < n && is_alnum(s[j + m])){ j += m - 1; continue; }
            return j;
        }
        if(m) j += m - 1;
    }
    return 0xFFFFFFFFu;
}

static void inline_parse(const char *s, u32 n, runs_t *rs, u8 st, const char *href, u32 hn, int depth){
    u32 i = 0, plain = 0;
    #define FLUSH() do { if(i > plain) add_run(rs, st, s + plain, i - plain, href, hn); } while(0)
    while(i < n){
        char c = s[i];
        if(c == '\\' && i + 1 < n && is_punct(s[i + 1])){
            FLUSH(); add_run(rs, st, s + i + 1, 1, href, hn); i += 2; plain = i; continue;
        }
        if(c == '\n'){
            /* two spaces (or a backslash) before a newline: a hard break; otherwise a space */
            int hard = (i >= 2 && s[i - 1] == ' ' && s[i - 2] == ' ');
            u32 e = i; while(e > plain && s[e - 1] == ' ') e--;
            if(e > plain) add_run(rs, st, s + plain, e - plain, href, hn);
            if(hard) add_run(rs, (u8)(st | MS_BR), "", 0, href, hn);
            else add_run(rs, st, " ", 1, href, hn);
            i++; while(i < n && is_sp(s[i])) i++;
            plain = i; continue;
        }
        if(c == '`'){
            u32 k = 0; while(i + k < n && s[i + k] == '`') k++;
            u32 j = i + k, close = 0xFFFFFFFFu;
            while(j + k <= n){
                if(s[j] == '`'){ u32 m = 0; while(j + m < n && s[j + m] == '`') m++; if(m == k){ close = j; break; } j += m; }
                else j++;
            }
            if(close != 0xFFFFFFFFu){
                FLUSH();
                u32 a = i + k, b = close;
                if(b - a >= 2 && s[a] == ' ' && s[b - 1] == ' '){ a++; b--; }
                add_run(rs, (u8)(st | MS_CODE), s + a, b - a, href, hn);
                i = close + k; plain = i; continue;
            }
            i += k; continue;
        }
        if((c == '*' || c == '_') && depth < 8){
            u32 k = 0; while(i + k < n && s[i + k] == c) k++;
            int left_ok = i + k < n && !is_sp(s[i + k]) && !(c == '_' && i > 0 && is_alnum(s[i - 1]));
            if(left_ok){
                u32 use = k > 3 ? 3 : k;
                u32 close = find_close(s, n, i + use, c, use);
                if(close == 0xFFFFFFFFu && use == 3){ use = 2; close = find_close(s, n, i + use, c, use); }
                if(close == 0xFFFFFFFFu && use >= 2){ use = 1; close = find_close(s, n, i + use, c, use); }
                if(close != 0xFFFFFFFFu){
                    FLUSH();
                    u8 add = (u8)((use >= 2 ? MS_B : 0) | (use != 2 ? MS_I : 0));
                    inline_parse(s + i + use, close - i - use, rs, (u8)(st | add), href, hn, depth + 1);
                    i = close + use; plain = i; continue;
                }
            }
            i += k; continue;
        }
        if(c == '~' && i + 1 < n && s[i + 1] == '~' && depth < 8){
            u32 close = find_close(s, n, i + 2, '~', 2);
            if(close != 0xFFFFFFFFu){
                FLUSH();
                inline_parse(s + i + 2, close - i - 2, rs, (u8)(st | MS_S), href, hn, depth + 1);
                i = close + 2; plain = i; continue;
            }
            i += 2; continue;
        }
        if((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) && depth < 8){
            int img = c == '!';
            u32 a = i + (img ? 2 : 1), j = a; int d = 1;
            while(j < n && d){ if(s[j] == '\\'){ j += 2; continue; } if(s[j] == '[') d++; else if(s[j] == ']') d--; if(d) j++; }
            if(j < n && j + 1 < n && s[j + 1] == '('){
                u32 u = j + 2, e = u; int pd = 1;
                while(e < n && pd){ if(s[e] == '(') pd++; else if(s[e] == ')') pd--; if(pd) e++; }
                if(e < n){
                    /* url, maybe "title" after it */
                    u32 us = u; while(us < e && is_sp(s[us])) us++;
                    u32 ue = us;
                    if(us < e && s[us] == '<'){ us++; ue = us; while(ue < e && s[ue] != '>') ue++; }
                    else while(ue < e && !is_sp(s[ue])) ue++;
                    FLUSH();
                    if(img) add_run(rs, (u8)(st | MS_IMG), s + a, j - a, s + us, ue - us);
                    else inline_parse(s + a, j - a, rs, (u8)(st | MS_LINK), s + us, ue - us, depth + 1);
                    i = e + 1; plain = i; continue;
                }
            }
            i += img ? 2 : 1; continue;
        }
        if(c == '<'){
            u32 e = i + 1;
            while(e < n && s[e] != '>' && !is_sp(s[e])) e++;
            int url = e < n && s[e] == '>' && (e - i > 8) && (!strncmp(s + i + 1, "http://", 7) || !strncmp(s + i + 1, "https://", 8) || !strncmp(s + i + 1, "mailto:", 7));
            if(url){
                FLUSH();
                add_run(rs, (u8)(st | MS_LINK), s + i + 1, e - i - 1, s + i + 1, e - i - 1);
                i = e + 1; plain = i; continue;
            }
        }
        if(c == 'h' && !(st & MS_LINK) && (i == 0 || !is_alnum(s[i - 1])) &&
           (!strncmp(s + i, "http://", 7 < n - i ? 7 : n - i) || !strncmp(s + i, "https://", 8 < n - i ? 8 : n - i)) && n - i > 8){
            u32 e = i;
            while(e < n && !is_sp(s[e]) && s[e] != '\n' && s[e] != '<' && s[e] != '>') e++;
            while(e > i && (s[e - 1] == '.' || s[e - 1] == ',' || s[e - 1] == ')' || s[e - 1] == ';' || s[e - 1] == ':')) e--;
            FLUSH();
            add_run(rs, (u8)(st | MS_LINK), s + i, e - i, s + i, e - i);
            i = e; plain = i; continue;
        }
        i++;
    }
    FLUSH();
    #undef FLUSH
}

/* ================= outputs ================= */

enum { B_PARA, B_H, B_CODE, B_HR, B_QUOTE_IN, B_QUOTE_OUT, B_LIST_IN, B_LIST_OUT, B_ITEM, B_TABLE, B_HTML };

typedef struct {
    int html;                /* 1: HTML into out; 0: drawing */
    buf_t out;
    /* drawing */
    md_draw_t *d;
    int x0, w, y;            /* the column being filled */
    int indent;              /* list indent (px) */
    int quote_depth;
    int quote_top[8];
    int list_depth;
    int list_num[8], list_ord[8];
    /* HTML only: list nesting, and whether that level's <li> is still open. A nested list
       has to be emitted before its parent's </li>, so the close is deferred here. */
    int hlist_depth;
    unsigned char hlist_li[8];
} md_t;

/* ---- drawing: words and lines ---- */
typedef struct { int x; int line_h; int any; } line_t;

static int dclip(md_t *m, int y, int h){ return y + h > m->d->clip_top && y < m->d->clip_bottom; }

static void draw_text(md_t *m, int x, int y, const char *s, u32 n, u32 color, int scale, int bold){
    char tmp[256];
    if(n > 255) n = 255;
    memcpy(tmp, s, n); tmp[n] = 0;
    if(dclip(m, y, 16 * scale)) m->d->text(m->d->ctx, x, y, tmp, color, scale, bold);
}
static void draw_rect(md_t *m, int x, int y, int w, int h, u32 c){
    if(w > 0 && h > 0 && dclip(m, y, h)) m->d->rect(m->d->ctx, x, y, w, h, c);
}

/* Lays runs out as wrapped lines from the current position; scale for the text. */
static void draw_runs(md_t *m, runs_t *rs, int scale, int bold_all, u32 color, int left){
    int cw = 8 * scale, lh = 16 * scale + 4 * scale / 2 + 2;
    int x = left, right = m->x0 + m->w;
    int pending_space = 0;
    for(int r = 0; r < rs->n; r++){
        run_t *ru = &rs->r[r];
        if(ru->st & MS_BR){ x = left; m->y += lh; pending_space = 0; continue; }
        u32 col = color;
        int bold = bold_all || (ru->st & MS_B);
        if(ru->st & MS_LINK) col = m->d->accent;
        if(ru->st & MS_CODE) col = m->d->code_fg;
        const char *t = ru->t; u32 n = ru->n;
        if(ru->st & MS_IMG){
            /* pictures are not drawn here: their alt text, marked */
            static char alt[200];
            u32 k = 0;
            const char *pre = "[image: ";
            while(*pre && k < 190) alt[k++] = *pre++;
            for(u32 q = 0; q < n && k < 196; q++) alt[k++] = t[q];
            alt[k++] = ']';
            t = alt; n = k; col = m->d->dim;
        }
        u32 i = 0;
        while(i < n){
            /* one word (or the spaces before it) at a time */
            if(t[i] == ' ' || t[i] == '\t'){ pending_space = 1; i++; continue; }
            u32 b = i;
            while(i < n && t[i] != ' ' && t[i] != '\t') i++;
            u32 wl = i - b;
            int ww = (int)wl * cw;
            int sp = (pending_space && x > left) ? cw : 0;
            if(x + sp + ww > right && x > left){ x = left; m->y += lh; sp = 0; }
            /* a word longer than the line is cut */
            while(ww > right - left && wl > 1){
                u32 fit = (u32)((right - x) / cw); if(fit < 1) fit = 1; if(fit > wl) fit = wl;
                draw_text(m, x, m->y, t + b, fit, col, scale, bold);
                b += fit; wl -= fit; ww = (int)wl * cw;
                x = left; m->y += lh;
            }
            x += sp;
            if(ru->st & MS_CODE) draw_rect(m, x - 2, m->y - 1, ww + 4, 16 * scale + 2, m->d->code_bg);
            draw_text(m, x, m->y, t + b, wl, col, scale, bold);
            if(ru->st & MS_LINK) draw_rect(m, x, m->y + 16 * scale - 2, ww, scale > 1 ? 2 : 1, col);
            if(ru->st & MS_S) draw_rect(m, x, m->y + 8 * scale, ww, scale > 1 ? 2 : 1, col);
            x += ww;
            pending_space = 0;
        }
    }
    m->y += lh;
}

static void draw_inline(md_t *m, const char *s, u32 n, int scale, int bold, u32 color, int left){
    runs_t rs; rs.cap = 2048; rs.n = 0;
    rs.r = kmalloc(sizeof(run_t) * (u32)rs.cap);
    if(!rs.r) return;
    inline_parse(s, n, &rs, 0, NULL, 0, 0);
    draw_runs(m, &rs, scale, bold, color, left);
    kfree(rs.r);
}

/* ---- HTML ---- */
static void html_inline(md_t *m, const char *s, u32 n){
    runs_t rs; rs.cap = 4096; rs.n = 0;
    rs.r = kmalloc(sizeof(run_t) * (u32)rs.cap);
    if(!rs.r){ put_esc(&m->out, s, n); return; }
    inline_parse(s, n, &rs, 0, NULL, 0, 0);
    buf_t *o = &m->out;
    for(int r = 0; r < rs.n; r++){
        run_t *ru = &rs.r[r];
        if(ru->st & MS_BR){ put(o, "<br>"); continue; }
        if(ru->st & MS_IMG){
            put(o, "<img src=\""); put_esc(o, ru->href, ru->hn); put(o, "\" alt=\""); put_esc(o, ru->t, ru->n); put(o, "\">");
            continue;
        }
        if(ru->st & MS_LINK){ put(o, "<a href=\""); put_esc(o, ru->href, ru->hn); put(o, "\">"); }
        if(ru->st & MS_B) put(o, "<strong>");
        if(ru->st & MS_I) put(o, "<em>");
        if(ru->st & MS_S) put(o, "<del>");
        if(ru->st & MS_CODE) put(o, "<code>");
        put_esc(o, ru->t, ru->n);
        if(ru->st & MS_CODE) put(o, "</code>");
        if(ru->st & MS_S) put(o, "</del>");
        if(ru->st & MS_I) put(o, "</em>");
        if(ru->st & MS_B) put(o, "</strong>");
        if(ru->st & MS_LINK) put(o, "</a>");
    }
    kfree(rs.r);
}

/* ---- the events, for both outputs ---- */

/* Close the pending <li> of the innermost open list in HTML mode. */
static void html_close_item(md_t *m){
    if(m->hlist_depth > 0 && m->hlist_depth <= 8 && m->hlist_li[m->hlist_depth - 1]){
        put(&m->out, "</li>\n");
        m->hlist_li[m->hlist_depth - 1] = 0;
    }
}

static void ev_heading(md_t *m, int level, const char *s, u32 n){
    if(m->html){
        char t[8] = "<h1>"; t[2] = (char)('0' + level);
        put(&m->out, t); html_inline(m, s, n);
        char e[8] = "</h1>\n"; e[3] = (char)('0' + level);
        put(&m->out, e);
        return;
    }
    int sc = m->d->scale;
    int hs = level == 1 ? sc * 2 : level == 2 ? sc + (sc > 1 ? 1 : 1) : sc;
    if(level == 1 && sc == 1) hs = 2;
    m->y += 8 * sc;
    draw_inline(m, s, n, hs, 1, m->d->fg, m->x0 + m->indent);
    if(level <= 2){ draw_rect(m, m->x0 + m->indent, m->y, m->w - m->indent, 1, m->d->rule); m->y += 4 * sc; }
    m->y += 4 * sc;
}

static void ev_para(md_t *m, const char *s, u32 n){
    if(m->html){ put(&m->out, "<p>"); html_inline(m, s, n); put(&m->out, "</p>\n"); return; }
    draw_inline(m, s, n, m->d->scale, 0, m->d->fg, m->x0 + m->indent);
    m->y += 6 * m->d->scale;
}

static void ev_code(md_t *m, const char *s, u32 n){
    if(m->html){ put(&m->out, "<pre><code>"); put_esc(&m->out, s, n); put(&m->out, "</code></pre>\n"); return; }
    int sc = m->d->scale, cw = 8 * sc, lh = 16 * sc + 2;
    int left = m->x0 + m->indent, width = m->w - m->indent;
    int top = m->y;
    /* the box first (its height is known from the lines and the width) */
    int cols = (width - 16 * sc) / cw; if(cols < 4) cols = 4;
    int lines = 0;
    for(u32 i = 0, col = 0; i <= n; i++){
        if(i == n || s[i] == '\n'){ lines++; col = 0; continue; }
        if((int)++col > cols){ lines++; col = 1; }
    }
    draw_rect(m, left, top, width, lines * lh + 12 * sc, m->d->code_bg);
    int y = top + 6 * sc;
    u32 b = 0;
    for(u32 i = 0; i <= n; i++){
        if(i == n || s[i] == '\n' || (int)(i - b) == cols){
            /* tabs as 4 spaces */
            char line[512]; u32 k = 0;
            for(u32 q = b; q < i && k < 500; q++){ if(s[q] == '\t'){ for(int t = 0; t < 4; t++) line[k++] = ' '; } else line[k++] = s[q]; }
            draw_text(m, left + 8 * sc, y, line, k, m->d->code_fg, sc, 0);
            y += lh;
            b = (i < n && s[i] == '\n') ? i + 1 : i;
        }
    }
    m->y = top + lines * lh + 12 * sc + 8 * sc;
}

static void ev_hr(md_t *m){
    if(m->html){ put(&m->out, "<hr>\n"); return; }
    m->y += 6 * m->d->scale;
    draw_rect(m, m->x0 + m->indent, m->y, m->w - m->indent, 2, m->d->rule);
    m->y += 10 * m->d->scale;
}

static void ev_quote(md_t *m, int open){
    if(m->html){ put(&m->out, open ? "<blockquote>\n" : "</blockquote>\n"); return; }
    int sc = m->d->scale;
    if(open){
        if(m->quote_depth < 8) m->quote_top[m->quote_depth] = m->y;
        m->quote_depth++;
        m->indent += 16 * sc;
    } else {
        m->quote_depth--;
        m->indent -= 16 * sc;
        int top = m->quote_depth < 8 ? m->quote_top[m->quote_depth] : m->y;
        draw_rect(m, m->x0 + m->indent + 2 * sc, top, 3 * sc, m->y - top - 4 * sc, m->d->quote);
    }
}

static void ev_list(md_t *m, int open, int ordered, int start){
    if(m->html){
        if(open){
            put(&m->out, ordered ? "<ol" : "<ul");
            if(ordered && start != 1){ put(&m->out, " start=\""); put_num(&m->out, start); put(&m->out, "\""); }
            put(&m->out, ">\n");
            if(m->hlist_depth < 8) m->hlist_li[m->hlist_depth] = 0;
            m->hlist_depth++;
        } else {
            if(m->hlist_depth > 0){
                /* a nested list is still inside its parent's <li>: finish that item first */
                html_close_item(m);
                m->hlist_depth--;
            }
            put(&m->out, ordered ? "</ol>\n" : "</ul>\n");
        }
        return;
    }
    int sc = m->d->scale;
    if(open){
        if(m->list_depth < 8){ m->list_num[m->list_depth] = start; m->list_ord[m->list_depth] = ordered; }
        m->list_depth++;
        m->indent += 24 * sc;
    } else {
        m->list_depth--;
        m->indent -= 24 * sc;
        if(m->list_depth == 0) m->y += 4 * sc;
    }
}

static void ev_item(md_t *m, int task, const char *s, u32 n){
    if(m->html){
        /* finish the previous item at this level, then leave this one's </li> open: a
           following nested list belongs inside this <li>, not after it. */
        html_close_item(m);
        put(&m->out, "<li>");
        if(m->hlist_depth > 0 && m->hlist_depth <= 8) m->hlist_li[m->hlist_depth - 1] = 1;
        if(task) put(&m->out, task == 2 ? "<input type=\"checkbox\" checked disabled> " : "<input type=\"checkbox\" disabled> ");
        html_inline(m, s, n);
        return;
    }
    int sc = m->d->scale, cw = 8 * sc;
    int lvl = m->list_depth - 1; if(lvl < 0) lvl = 0; if(lvl > 7) lvl = 7;
    char mark[16]; u32 k = 0;
    if(m->list_ord[lvl]){
        int v = m->list_num[lvl]++; char t[12]; int tk = 0;
        do { t[tk++] = (char)('0' + v % 10); v /= 10; } while(v && tk < 10);
        while(tk) mark[k++] = t[--tk];
        mark[k++] = '.';
    } else mark[k++] = lvl == 0 ? '\x07' : lvl == 1 ? 'o' : '-';
    int left = m->x0 + m->indent;
    draw_text(m, left - (int)k * cw - cw / 2, m->y, mark, k, m->d->dim, sc, 0);
    if(task){
        draw_text(m, left, m->y, task == 2 ? "[x]" : "[ ]", 3, task == 2 ? m->d->accent : m->d->dim, sc, 0);
        left += 4 * cw;
    }
    draw_inline(m, s, n, sc, 0, m->d->fg, left);
    m->y += 2 * sc;
}

/* a table: cells split by '|' in each line */
static int split_cells(const char *s, u32 n, const char **cs, u32 *cl, int max){
    u32 i = 0;
    while(i < n && is_sp(s[i])) i++;
    if(i < n && s[i] == '|') i++;
    u32 e = n; while(e > i && is_sp(s[e - 1])) e--;
    if(e > i && s[e - 1] == '|' && !(e > 1 && s[e - 2] == '\\')) e--;
    int k = 0; u32 b = i;
    for(u32 j = i; j <= e && k < max; j++){
        if(j < e && s[j] == '\\'){ j++; continue; }
        if(j == e || s[j] == '|'){
            u32 a = b, z = j;
            while(a < z && is_sp(s[a])) a++;
            while(z > a && is_sp(s[z - 1])) z--;
            cs[k] = s + a; cl[k] = z - a; k++;
            b = j + 1;
        }
    }
    return k;
}

static void ev_table(md_t *m, const char **lines, u32 *lens, int nl, const int *align){
    const char *cs[16]; u32 cl[16];
    int ncol = split_cells(lines[0], lens[0], cs, cl, 16);
    if(ncol < 1) return;
    if(m->html){
        put(&m->out, "<table>\n<thead><tr>");
        for(int c = 0; c < ncol; c++){
            put(&m->out, align[c] == 1 ? "<th align=\"center\">" : align[c] == 2 ? "<th align=\"right\">" : "<th>");
            html_inline(m, cs[c], cl[c]); put(&m->out, "</th>");
        }
        put(&m->out, "</tr></thead>\n<tbody>\n");
        for(int r = 2; r < nl; r++){
            int k = split_cells(lines[r], lens[r], cs, cl, 16);
            put(&m->out, "<tr>");
            for(int c = 0; c < ncol; c++){
                put(&m->out, align[c] == 1 ? "<td align=\"center\">" : align[c] == 2 ? "<td align=\"right\">" : "<td>");
                if(c < k) html_inline(m, cs[c], cl[c]);
                put(&m->out, "</td>");
            }
            put(&m->out, "</tr>\n");
        }
        put(&m->out, "</tbody></table>\n");
        return;
    }
    int sc = m->d->scale;
    int left = m->x0 + m->indent, width = m->w - m->indent;
    int colw = width / ncol;
    int pad = 6 * sc;
    for(int r = 0; r < nl; r++){
        if(r == 1) continue;                           /* the --- line */
        int k = split_cells(lines[r], lens[r], cs, cl, 16);
        int top = m->y, bottom = top;
        int sx0 = m->x0, sw = m->w;
        for(int c = 0; c < ncol; c++){
            m->x0 = left + c * colw + pad; m->w = colw - 2 * pad;
            int save_indent = m->indent; m->indent = 0;
            m->y = top + pad;
            if(c < k) draw_inline(m, cs[c], cl[c], sc, r == 0, m->d->fg, m->x0);
            else m->y += 16 * sc;
            if(m->y > bottom) bottom = m->y;
            m->indent = save_indent;
        }
        m->x0 = sx0; m->w = sw;
        bottom += pad / 2;
        draw_rect(m, left, bottom, width, 1, m->d->rule);
        if(r == 0) draw_rect(m, left, top, width, 1, m->d->rule);
        for(int c = 0; c <= ncol; c++) draw_rect(m, left + (c == ncol ? width - 1 : c * colw), top, 1, bottom - top, m->d->rule);
        m->y = bottom + 1;
    }
    m->y += 6 * sc;
}

static void ev_html(md_t *m, const char *s, u32 n){
    if(m->html){ put_n(&m->out, s, n); put(&m->out, "\n"); return; }
    /* drawn: the text between the tags */
    char *t = kmalloc(n + 1);
    if(!t) return;
    u32 k = 0; int in_tag = 0;
    for(u32 i = 0; i < n; i++){
        if(s[i] == '<'){ in_tag = 1; continue; }
        if(s[i] == '>'){ in_tag = 0; t[k++] = ' '; continue; }
        if(!in_tag) t[k++] = s[i];
    }
    u32 a = 0; while(a < k && (t[a] == ' ' || t[a] == '\n')) a++;
    if(k > a) ev_para(m, t + a, k - a);
    kfree(t);
}

/* ================= blocks ================= */

typedef struct { const char *s; u32 n; } line_s;

static u32 indent_of(const char *s, u32 n){
    u32 w = 0;
    for(u32 i = 0; i < n; i++){ if(s[i] == ' ') w++; else if(s[i] == '\t') w += 4 - w % 4; else break; }
    return w;
}
static const char *skip_ws(const char *s, u32 n, u32 *left){
    u32 i = 0; while(i < n && is_sp(s[i])) i++;
    *left = n - i; return s + i;
}
static int blank_line(const char *s, u32 n){ for(u32 i = 0; i < n; i++) if(!is_sp(s[i]) && s[i] != '\r') return 0; return 1; }

/* number of leading '>' markers on a line (spaces are allowed before each one) */
static int quote_level(const char *s, u32 n){
    int lev = 0; u32 i = 0;
    while(i < n){
        u32 j = i; while(j < n && is_sp(s[j])) j++;
        if(j < n && s[j] == '>'){ lev++; i = j + 1; }
        else break;
    }
    return lev;
}
/* offset after removing qlev markers and one following space */
static u32 quote_strip(const char *s, u32 n, int qlev){
    u32 i = 0;
    for(int k = 0; k < qlev; k++){
        while(i < n && is_sp(s[i])) i++;
        if(i < n && s[i] == '>') i++; else break;
    }
    if(i < n && is_sp(s[i])) i++;
    return i;
}

static int is_hr(const char *s, u32 n){
    u32 l; const char *t = skip_ws(s, n, &l);
    if(!l || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return 0;
    int cnt = 0;
    for(u32 i = 0; i < l; i++){ if(t[i] == t[0]) cnt++; else if(!is_sp(t[i]) && t[i] != '\r') return 0; }
    return cnt >= 3;
}
/* "# x" -> level, *text set; 0 if not a heading */
static int atx(const char *s, u32 n, const char **t, u32 *tl){
    u32 l; const char *p = skip_ws(s, n, &l);
    if(indent_of(s, n) > 3) return 0;
    int lv = 0; while((u32)lv < l && p[lv] == '#') lv++;
    if(lv < 1 || lv > 6 || ((u32)lv < l && !is_sp(p[lv]))) return 0;
    const char *a = p + lv; u32 al = l - (u32)lv;
    while(al && is_sp(*a)){ a++; al--; }
    while(al && (is_sp(a[al - 1]) || a[al - 1] == '\r')) al--;
    while(al && a[al - 1] == '#') al--;                 /* closing #s */
    while(al && is_sp(a[al - 1])) al--;
    *t = a; *tl = al;
    return lv;
}
/* a list marker: returns the content offset, 0 if none; *ordered, *num */
static u32 list_marker(const char *s, u32 n, int *ordered, int *num){
    u32 i = 0; while(i < n && is_sp(s[i])) i++;
    if(i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < n && is_sp(s[i + 1])){
        *ordered = 0; *num = 0;
        u32 c = i + 2; while(c < n && is_sp(s[c])) c++;
        return c;
    }
    u32 d = i; int v = 0;
    while(d < n && is_digit(s[d]) && d - i < 9){ v = v * 10 + (s[d] - '0'); d++; }
    if(d > i && d + 1 < n && (s[d] == '.' || s[d] == ')') && is_sp(s[d + 1])){
        *ordered = 1; *num = v;
        u32 c = d + 2; while(c < n && is_sp(s[c])) c++;
        return c;
    }
    return 0;
}
static int fence(const char *s, u32 n, char *ch, u32 *len){
    u32 l; const char *p = skip_ws(s, n, &l);
    if(indent_of(s, n) > 3 || l < 3 || (p[0] != '`' && p[0] != '~')) return 0;
    u32 k = 0; while(k < l && p[k] == p[0]) k++;
    if(k < 3) return 0;
    *ch = p[0]; *len = k;
    return 1;
}
static int table_sep(const char *s, u32 n, int *align, int max){
    const char *cs[16]; u32 cl[16];
    int k = split_cells(s, n, cs, cl, 16);
    if(k < 1) return 0;
    int any_pipe = 0; for(u32 i = 0; i < n; i++) if(s[i] == '|') any_pipe = 1;
    if(!any_pipe) return 0;
    for(int c = 0; c < k; c++){
        const char *t = cs[c]; u32 l = cl[c];
        if(!l) return 0;
        int lc = t[0] == ':', rc = t[l - 1] == ':';
        for(u32 i = lc; i < l - (u32)rc; i++) if(t[i] != '-') return 0;
        if(c < max) align[c] = lc && rc ? 1 : rc ? 2 : 0;
    }
    return k;
}
static int html_block_start(const char *s, u32 n){
    u32 l; const char *p = skip_ws(s, n, &l);
    if(l < 3 || p[0] != '<') return 0;
    if(p[1] == '!' || p[1] == '/') return 1;
    static const char *tags[] = { "div", "p", "table", "img", "a ", "a>", "details", "summary", "h1", "h2", "h3", "h4", "picture", "center", "br", "hr", "pre", "ul", "ol", "kbd", "sup", "section", "video", 0 };
    for(int i = 0; tags[i]; i++){ u32 tl = (u32)strlen(tags[i]); if(l > tl && !strncmp(p + 1, tags[i], tl)) return 1; }
    return 0;
}

static void close_lists(md_t *m, int *open, int *ord, int to){
    while(*open > to){ (*open)--; ev_list(m, 0, ord[*open], 0); }
}

static void blocks(md_t *m, const char *src, u32 len, int depth);

/* Parses lines[0..nl) into blocks. */
static void blocks_lines(md_t *m, line_s *L, int nl, int depth){
    int lopen = 0, lord[8], lindent[8];
    int i = 0;
    while(i < nl){
        const char *s = L[i].s; u32 n = L[i].n;
        while(n && s[n - 1] == '\r') n--;
        if(blank_line(s, n)){ i++; continue; }
        int ordered, num;
        u32 lm = list_marker(s, n, &ordered, &num);
        u32 ind = indent_of(s, n);
        if(!lm && lopen && ind < (u32)(lindent[lopen - 1] + 2)) close_lists(m, &lopen, lord, 0);
        if(!lm && lopen && ind >= (u32)(lindent[lopen - 1] + 2)){
            /* a continuation paragraph inside an item, after a blank line */
            close_lists(m, &lopen, lord, lopen);
        }
        char fc; u32 fl;
        const char *t; u32 tl;
        int lv;
        if(fence(s, n, &fc, &fl)){
            if(!lm) close_lists(m, &lopen, lord, 0);
            int j = i + 1;
            while(j < nl){ char c2; u32 l2; if(fence(L[j].s, L[j].n, &c2, &l2) && c2 == fc && l2 >= fl) break; j++; }
            if(j > i + 1){
                const char *a = L[i + 1].s; const char *e = L[j - 1].s + L[j - 1].n;
                u32 cn = (u32)(e - a); while(cn && a[cn - 1] == '\r') cn--;
                ev_code(m, a, cn);
            } else ev_code(m, "", 0);
            i = j + 1;
            continue;
        }
        if(ind >= 4 && !lopen){
            /* indented code: strip four columns from each line */
            buf_t b = { 0, 0, 0, 0 };
            int j = i;
            while(j < nl && (indent_of(L[j].s, L[j].n) >= 4 || blank_line(L[j].s, L[j].n))){
                const char *ls = L[j].s; u32 ln = L[j].n, k = 0, col = 0;
                while(k < ln && col < 4){ if(ls[k] == '\t') col += 4; else col++; k++; }
                if(b.n) put(&b, "\n");
                put_n(&b, ls + k, ln - k);
                j++;
            }
            while(b.n && (b.b[b.n - 1] == '\n' || b.b[b.n - 1] == '\r')) b.n--;
            if(b.b){ ev_code(m, b.b, b.n); kfree(b.b); }
            i = j;
            continue;
        }
        if((lv = atx(s, n, &t, &tl)) > 0){ close_lists(m, &lopen, lord, 0); ev_heading(m, lv, t, tl); i++; continue; }
        if(is_hr(s, n) && !lm){ close_lists(m, &lopen, lord, 0); ev_hr(m); i++; continue; }
        {
            u32 l; const char *p = skip_ws(s, n, &l);
            if(l && p[0] == '>'){
                close_lists(m, &lopen, lord, 0);
                /* Collect the quote's lines with exactly qlev markers removed. Keeping deeper
                   markers intact lets the recursive parse see nested quotes. A line that drops
                   back from a deeper level to this one starts a new block: insert a blank line
                   so it is not swallowed as a lazy continuation of the nested quote. */
                int base = quote_level(s, n); if(base < 1) base = 1;
                buf_t b = { 0, 0, 0, 0 };
                int j = i, prev = base;
                while(j < nl && !blank_line(L[j].s, L[j].n)){
                    int qlev = quote_level(L[j].s, L[j].n);
                    if(qlev < base){                 /* belongs to an outer quote, or is lazy */
                        if(qlev > 0) break;          /* a real outer marker ends this quote */
                        if(prev > base) break;       /* lazy line after a nested block: stop */
                    }
                    if(b.n) put(&b, (prev > base && qlev <= base) ? "\n\n" : "\n");
                    u32 off = quote_strip(L[j].s, L[j].n, base);
                    put_n(&b, L[j].s + off, L[j].n - off);
                    prev = qlev;
                    j++;
                }
                ev_quote(m, 1);
                if(b.b && depth < 6) blocks(m, b.b, b.n, depth + 1);
                ev_quote(m, 0);
                if(b.b) kfree(b.b);
                i = j;
                continue;
            }
        }
        if(html_block_start(s, n)){
            close_lists(m, &lopen, lord, 0);
            int j = i;
            while(j < nl && !blank_line(L[j].s, L[j].n)) j++;
            const char *a = L[i].s, *e = L[j - 1].s + L[j - 1].n;
            ev_html(m, a, (u32)(e - a));
            i = j;
            continue;
        }
        if(lm){
            /* a list item: its depth from the indentation */
            int lvl = 0;
            while(lvl < lopen && (int)ind >= lindent[lvl] + 2) lvl++;
            close_lists(m, &lopen, lord, lvl + 1 > lopen ? lopen : lvl + 1);
            if(lopen > lvl && lord[lvl] != ordered) close_lists(m, &lopen, lord, lvl);
            if(lopen <= lvl && lopen < 8){ lord[lopen] = ordered; lindent[lopen] = (int)ind; ev_list(m, 1, ordered, num ? num : 1); lopen++; }
            /* its text: this line and the lines that continue it */
            const char *a = s + lm; u32 al = n - lm;
            int task = 0;
            if(al >= 3 && a[0] == '[' && (a[1] == ' ' || a[1] == 'x' || a[1] == 'X') && a[2] == ']'){
                task = a[1] == ' ' ? 1 : 2; a += 3; al -= 3; while(al && is_sp(*a)){ a++; al--; }
            }
            int j = i + 1;
            while(j < nl){
                const char *s2 = L[j].s; u32 n2 = L[j].n;
                int o2, n2v;
                if(blank_line(s2, n2) || list_marker(s2, n2, &o2, &n2v) || atx(s2, n2, &t, &tl) || is_hr(s2, n2) || fence(s2, n2, &fc, &fl)) break;
                u32 l2; const char *p2 = skip_ws(s2, n2, &l2);
                if(l2 && p2[0] == '>') break;
                j++;
            }
            const char *e = L[j - 1].s + L[j - 1].n;
            u32 en = (u32)(e - a);
            while(en && (a[en - 1] == '\r' || is_sp(a[en - 1]))) en--;
            ev_item(m, task, a, en);
            i = j;
            continue;
        }
        close_lists(m, &lopen, lord, 0);
        /* a table: a header line and a --- line under it */
        int align[16] = { 0 };
        if(i + 1 < nl){
            int hc = 0; for(u32 q = 0; q < n; q++) if(s[q] == '|') hc = 1;
            if(hc && table_sep(L[i + 1].s, L[i + 1].n, align, 16)){
                int j = i + 2;
                while(j < nl && !blank_line(L[j].s, L[j].n)){ int p = 0; for(u32 q = 0; q < L[j].n; q++) if(L[j].s[q] == '|') p = 1; if(!p) break; j++; }
                const char *ls[256]; u32 ln[256]; int k = 0;
                for(int q = i; q < j && k < 256; q++){ ls[k] = L[q].s; ln[k] = L[q].n; while(ln[k] && ls[k][ln[k] - 1] == '\r') ln[k]--; k++; }
                ev_table(m, ls, ln, k, align);
                i = j;
                continue;
            }
        }
        /* a paragraph, to a blank line or the next block (or a setext underline) */
        int j = i + 1, setext = 0;
        while(j < nl){
            const char *s2 = L[j].s; u32 n2 = L[j].n;
            if(blank_line(s2, n2)) break;
            u32 l2; const char *p2 = skip_ws(s2, n2, &l2);
            while(l2 && (p2[l2 - 1] == '\r' || is_sp(p2[l2 - 1]))) l2--;
            if(l2 && (p2[0] == '=' || p2[0] == '-')){
                u32 q = 0; while(q < l2 && p2[q] == p2[0]) q++;
                if(q == l2){ setext = p2[0] == '=' ? 1 : 2; break; }
            }
            int o2, n2v;
            if(atx(s2, n2, &t, &tl) || is_hr(s2, n2) || fence(s2, n2, &fc, &fl) || (l2 && p2[0] == '>') || list_marker(s2, n2, &o2, &n2v) || html_block_start(s2, n2)) break;
            j++;
        }
        const char *e = L[j - 1].s + L[j - 1].n;
        u32 l; const char *a = skip_ws(s, n, &l);
        u32 pn = (u32)(e - a);
        while(pn && (a[pn - 1] == '\r' || is_sp(a[pn - 1]))) pn--;
        if(setext){ ev_heading(m, setext, a, pn); i = j + 1; }
        else { ev_para(m, a, pn); i = j; }
    }
    close_lists(m, &lopen, lord, 0);
}

static void blocks(md_t *m, const char *src, u32 len, int depth){
    int nl = 1;
    for(u32 i = 0; i < len; i++) if(src[i] == '\n') nl++;
    line_s *L = kmalloc(sizeof(line_s) * (u32)nl);
    if(!L) return;
    int k = 0; u32 b = 0;
    for(u32 i = 0; i <= len; i++){
        if(i == len || src[i] == '\n'){ L[k].s = src + b; L[k].n = i - b; k++; b = i + 1; }
    }
    blocks_lines(m, L, k, depth);
    kfree(L);
}

/* ================= entry points ================= */

char *md_to_html(const char *md, u32 len, u32 *out_len){
    md_t m;
    memset(&m, 0, sizeof m);
    m.html = 1;
    put(&m.out, "");
    blocks(&m, md, len, 0);
    if(m.out.oom){ if(m.out.b) kfree(m.out.b); return NULL; }
    if(out_len) *out_len = m.out.n;
    return m.out.b;
}

char *md_page(const char *md, u32 len, const char *title, u32 *out_len){
    u32 hl = 0;
    char *body = md_to_html(md, len, &hl);
    if(!body) return NULL;
    buf_t o = { 0, 0, 0, 0 };
    put(&o, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>");
    put_esc(&o, title ? title : "Markdown", title ? (u32)strlen(title) : 8);
    put(&o, "</title><style>"
        "body{margin:0;background:#fff;color:#1f2328;font-size:16px}"
        ".md{max-width:880px;margin:0 auto;padding:32px 24px}"
        "h1,h2{border-bottom:1px solid #d1d9e0;padding-bottom:.3em}"
        "h1,h2,h3,h4{margin:24px 0 16px 0}"
        "p,ul,ol,blockquote,table,pre{margin:0 0 16px 0}"
        "a{color:#0969da}"
        "code{background:#eff1f3;padding:0 4px;font-size:85%}"
        "pre{background:#f6f8fa;padding:16px;font-size:14px}pre code{background:#f6f8fa;padding:0;font-size:100%}"
        "blockquote{border-left:4px solid #d1d9e0;padding:0 16px;color:#59636e;margin-left:0}"
        "table{border:1px solid #d1d9e0}th,td{border:1px solid #d1d9e0;padding:6px 13px}th{background:#f6f8fa}"
        "hr{border:0;border-top:4px solid #d1d9e0}img{max-width:100%}"
        "</style></head><body><div class=\"md\">\n");
    put_n(&o, body, hl);
    put(&o, "</div></body></html>\n");
    kfree(body);
    if(o.oom){ if(o.b) kfree(o.b); return NULL; }
    if(out_len) *out_len = o.n;
    return o.b;
}

int md_draw(const char *md, u32 len, int x, int y, int w, md_draw_t *d){
    md_t m;
    memset(&m, 0, sizeof m);
    m.html = 0;
    m.d = d;
    if(d->scale < 1) d->scale = 1;
    m.x0 = x; m.w = w; m.y = y;
    blocks(&m, md, len, 0);
    return m.y - y;
}
