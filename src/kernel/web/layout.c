/* Layout: walks the styled tree and produces drawing commands. Block boxes stack
 * vertically; inline words flow left to right and wrap at the window width. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "string.h"

#define CMD_MAX    20000
#define ARENA_MAX  (512 * 1024)
#define FRAG_MAX   512
#define LINE_GAP   4

static web_cmd_t *cmds = NULL;
static int ncmds = 0;
static char *arena = NULL;
static u32 arena_n = 0;

typedef struct {
    const void *dom;
    int bg_set;
    u32 bg;
    int x, w;                 /* x within the line, width in px */
    int scale;
    u32 color;
    int bold, italic, underline, link;
    const char *text;         /* copy in the arena */
    int tlen;
} frag_t;

static frag_t frags[FRAG_MAX];
static int nfrag = 0;

static int view_w;           /* the width of the whole page (for vw) */
static int cx, cw, cy;       /* content box: left, width, current y */
static int used;             /* width used on the current line */
static int pending_space;
static int cur_align;
static int cur_link = -1;
static int cur_bg_set = 0;   /* background of the inline element being laid out */
static u32 cur_bg = 0;

/* ---- output ---- */

static void emit(const web_cmd_t *c){
    if(ncmds < CMD_MAX) cmds[ncmds++] = *c;
}

static const char *arena_put(const char *s, int n){
    if(arena_n + (u32)n + 1 > ARENA_MAX) return NULL;
    char *d = arena + arena_n;
    memcpy(d, s, (u32)n);
    d[n] = 0;
    arena_n += (u32)n + 1;
    return d;
}

static int scale_of(int size){
    int sc = (size + 8) / 16;
    return sc < 1 ? 1 : sc;
}

static void flush_line(void){
    if(nfrag == 0) return;
    int lh = 16;
    for(int i = 0; i < nfrag; i++){
        int h = 16 * frags[i].scale;
        if(h > lh) lh = h;
    }
    int offset = 0;
    if(cur_align == 1) offset = (cw - used) / 2;
    else if(cur_align == 2) offset = cw - used;
    if(offset < 0) offset = 0;
    for(int i = 0; i < nfrag; i++){
        frag_t *f = &frags[i];
        int top = cy + (lh - 16 * f->scale);
        if(f->bg_set){
            /* an inline box's background sits behind its words */
            web_cmd_t b = { .kind = WEB_CMD_RECT, .x = cx + offset + f->x, .y = top, .w = f->w, .h = 16 * f->scale,
                            .color = f->bg, .link = -1 };
            emit(&b);
        }
        web_cmd_t c = { .kind = WEB_CMD_TEXT, .x = cx + offset + f->x, .y = top, .w = f->w, .h = 16 * f->scale,
                        .color = f->color, .scale = f->scale, .bold = f->bold, .italic = f->italic, .text = f->text, .link = f->link,
                        .dom = f->dom };
        emit(&c);
        if(f->underline){
            web_cmd_t u = { .kind = WEB_CMD_RECT, .x = cx + offset + f->x, .y = top + 16 * f->scale - f->scale - 2,
                            .w = f->w, .h = 2, .color = f->color, .link = -1 };
            emit(&u);
        }
    }
    cy += lh + LINE_GAP;
    nfrag = 0;
    used = 0;
    pending_space = 0;
}

/* ---- words ---- */

static const void *cur_dom = NULL;

static void add_piece(const char *w, int wl, wstyle_t *st, int link){
    int sc = scale_of(st->size);
    int cwpx = 8 * sc;
    int space = (pending_space && nfrag > 0) ? cwpx : 0;
    if(nfrag > 0 && used + space + wl * cwpx > cw){ flush_line(); space = 0; }
    if(nfrag == FRAG_MAX) flush_line();
    /* a word wider than its box runs over the edge (as in a browser); only a word wider than
       the page itself is broken */
    int room = view_w - (cx > 0 ? cx : 0) - 8;
    if(room < cw) room = cw;
    while(wl * cwpx > room && wl > 1){
        int fit = room / cwpx;
        if(fit < 1) fit = 1;
        const char *t = arena_put(w, fit);
        if(!t) return;
        frags[nfrag++] = (frag_t){ cur_dom, cur_bg_set, cur_bg, used, fit * cwpx, sc, st->color, st->bold, st->italic, st->underline, link, t, fit };
        used += fit * cwpx;
        flush_line();
        w += fit; wl -= fit;
    }
    const char *t = arena_put(w, wl);
    if(!t) return;
    frags[nfrag++] = (frag_t){ cur_dom, cur_bg_set, cur_bg, used + space, wl * cwpx, sc, st->color, st->bold, st->italic, st->underline, link, t, wl };
    used += space + wl * cwpx;
    pending_space = 0;
}

/* The font has ASCII only: a close ASCII spelling for a Unicode character ("" drops it). */
const char *web_uni_ascii(u32 cp){
    static char one[2];
    if(cp >= 32 && cp < 127){ one[0] = (char)cp; one[1] = 0; return one; }
    if(cp >= 0xC0 && cp <= 0xFF){
        static const char fold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
        one[0] = fold[cp - 0xC0]; one[1] = 0;
        return one;
    }
    switch(cp){
        case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return " ";
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: case 0xFE0F: return "";
        case 0xA9: return "(c)";   case 0xAE: return "(R)";   case 0x2122: return "TM";
        case 0xB0: return "o";     case 0xB7: case 0x2022: case 0x2023: case 0x25CF: case 0x2219: return "*";
        case 0xAB: return "<<";    case 0xBB: return ">>";    case 0xD7: return "x";    case 0xF7: return "/";
        case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2026: return "...";
        case 0x2039: return "<";   case 0x203A: return ">";
        case 0x20AC: return "EUR"; case 0xA3: return "GBP";   case 0xA5: return "JPY";  case 0xA2: return "c";
        case 0x2190: return "<-";  case 0x2192: return "->";  case 0x2191: return "^";  case 0x2193: return "v";
        case 0x21D2: return "=>";  case 0x2194: return "<->";
        case 0x25B2: case 0x25B4: return "^";  case 0x25BC: case 0x25BE: return "v";
        case 0x25B6: case 0x25B8: return ">";  case 0x25C0: case 0x25C2: return "<";
        case 0x2713: case 0x2714: return "v";  case 0x2717: case 0x2718: return "x";
        case 0x2605: case 0x2606: return "*";
        case 0xBC: return "1/4";   case 0xBD: return "1/2";   case 0xBE: return "3/4";
        case 0xB1: return "+/-";   case 0x2264: return "<=";  case 0x2265: return ">=";  case 0x2260: return "!=";
        case 0xA7: return "S";     case 0xB6: return "P";     case 0xBF: return "?";    case 0xA1: return "!";
    }
    if(cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF)) return "";     /* emoji and symbols */
    return "?";
}

/* Decodes one UTF-8 character at s. Returns its length (at least 1). */
int web_utf8(const char *s, u32 n, u32 *cp){
    unsigned char b = (unsigned char)s[0];
    int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if(len == 1 || (u32)len > n){ *cp = b >= 0x80 && b < 0xC0 ? 0xFFFD : b; return 1; }
    u32 v = b & (0x7F >> len);
    for(int k = 1; k < len; k++){
        unsigned char c = (unsigned char)s[k];
        if((c & 0xC0) != 0x80){ *cp = 0xFFFD; return k; }
        v = (v << 6) | (c & 0x3F);
    }
    *cp = v;
    return len;
}

/* Decodes one character reference at s (points at '&') into out.
 * Returns the length used, 0 if there is none. */
int web_entity(const char *s, u32 n, const char **out){
    static const struct { const char *name; u32 cp; } e[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' },
        { "nbsp;", 0xA0 }, { "copy;", 0xA9 }, { "reg;", 0xAE }, { "deg;", 0xB0 }, { "ndash;", 0x2013 },
        { "mdash;", 0x2014 }, { "lsquo;", 0x2018 }, { "rsquo;", 0x2019 }, { "ldquo;", 0x201C }, { "rdquo;", 0x201D },
        { "eacute;", 0xE9 }, { "egrave;", 0xE8 }, { "ecirc;", 0xEA }, { "agrave;", 0xE0 }, { "auml;", 0xE4 },
        { "ouml;", 0xF6 }, { "uuml;", 0xFC }, { "ccedil;", 0xE7 }, { "ntilde;", 0xF1 }, { "hellip;", 0x2026 },
        { "bull;", 0x2022 }, { "middot;", 0xB7 }, { "raquo;", 0xBB }, { "laquo;", 0xAB }, { "times;", 0xD7 },
        { "rarr;", 0x2192 }, { "larr;", 0x2190 }, { "uarr;", 0x2191 }, { "darr;", 0x2193 }, { "trade;", 0x2122 },
        { "euro;", 0x20AC }, { "pound;", 0xA3 }, { "shy;", 0xAD }, { "zwj;", 0x200D }, { "thinsp;", 0x2009 },
        { "aacute;", 0xE1 }, { "iacute;", 0xED }, { "oacute;", 0xF3 }, { "uacute;", 0xFA }, { "szlig;", 0xDF },
    };
    for(u32 i = 0; i < sizeof e / sizeof e[0]; i++){
        u32 k = 0, nl = (u32)strlen(e[i].name);
        if(nl > n - 1) continue;
        while(k < nl && s[1 + k] == e[i].name[k]) k++;
        if(k == nl){ *out = web_uni_ascii(e[i].cp); return (int)nl + 1; }
    }
    if(n > 2 && s[1] == '#'){
        u32 v = 0, k = 2;
        int hex = k < n && (s[k] == 'x' || s[k] == 'X');
        if(hex) k++;
        u32 d0 = k;
        for(; k < n; k++){
            char c = s[k];
            if(c >= '0' && c <= '9') v = v * (hex ? 16 : 10) + (u32)(c - '0');
            else if(hex && c >= 'a' && c <= 'f') v = v * 16 + (u32)(c - 'a' + 10);
            else if(hex && c >= 'A' && c <= 'F') v = v * 16 + (u32)(c - 'A' + 10);
            else break;
        }
        if(k < n && s[k] == ';' && k > d0){
            *out = web_uni_ascii(v);
            return (int)k + 1;
        }
    }
    return 0;
}

/* Splits text into words (collapsing white space) and adds them. */
static void add_text(const char *s, u32 n, wstyle_t *st, int link){
    if(st->hide_text) return;
    if(st->white_pre){
        /* keep spaces and newlines: emit each line as pieces, break on '\n' */
        char buf[160]; int bl = 0;
        u32 i = 0;
        if(n > 0 && s[0] == '\n') i = 1;             /* a single leading newline after <pre> is dropped */
        pending_space = 0;
        for(; i < n; ){
            char c = s[i];
            if(c == '\r'){ i++; continue; }
            if(c == '\n'){ if(bl){ add_piece(buf, bl, st, link); bl = 0; } flush_line(); pending_space = 0; i++; continue; }
            if(c == '\t'){ i++; for(int k = 0; k < 4 && bl < 159; k++) buf[bl++] = ' '; }
            else if(c == '&'){
                const char *rep = NULL; int used = web_entity(s + i, n - i, &rep);
                if(used) i += (u32)used; else { rep = "&"; i++; }
                for(; rep && *rep; rep++) if(bl < 159) buf[bl++] = *rep;
            } else if((unsigned char)c >= 0x80){
                u32 cp; i += (u32)web_utf8(s + i, n - i, &cp);
                const char *rep = web_uni_ascii(cp);
                for(; rep && *rep; rep++) if(bl < 159) buf[bl++] = *rep;
            } else { if(bl < 159) buf[bl++] = c; i++; }
            if(bl >= 150){ add_piece(buf, bl, st, link); bl = 0; }   /* flush long lines in chunks */
        }
        if(bl) add_piece(buf, bl, st, link);
        return;
    }
    char word[160];
    int wl = 0;
    for(u32 i = 0; i < n; ){
        char c = s[i];
        int is_ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if(is_ws){
            if(wl){ add_piece(word, wl, st, link); wl = 0; }
            pending_space = 1;
            i++;
            continue;
        }
        const char *rep = NULL;
        if(c == '&'){
            int used_n = web_entity(s + i, n - i, &rep);
            if(used_n) i += (u32)used_n;
            else { rep = "&"; i++; }
        } else if((unsigned char)c >= 0x80){
            u32 cp;
            i += (u32)web_utf8(s + i, n - i, &cp);
            rep = web_uni_ascii(cp);
        } else {
            i++;
            if(wl < (int)sizeof word - 1) word[wl++] = c;
            continue;
        }
        /* a decoded no-break space stays inside the word */
        for(; *rep; rep++) if(wl < (int)sizeof word - 1) word[wl++] = *rep;
    }
    if(wl) add_piece(word, wl, st, link);
}

static void layout_node(wnode_t *n);

/* A flex row: each child is a column of the given width (or an equal share). */
static int blank_text(const wnode_t *c){
    for(u32 i = 0; i < c->tlen; i++){
        char ch = c->txt[i];
        if(ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return 0;
    }
    return 1;
}

/* A flex item's outer width when its CSS gives one (0 = none given). */
static int item_w(const wstyle_t *st, int w){
    int want = st->width + w * st->width_pct / 1000 + view_w * st->width_vw / 1000;
    if(want <= 0) return 0;
    int extra = st->border_box ? 0 : st->p[1] + st->p[3] + st->bl + st->br;
    int out = want + extra + st->m[1] + st->m[3];
    return out > w ? w : out;
}

/* How wide a node wants to be: on one line (max) and at its narrowest, the longest word (min).
 * An estimate from the text, without laying it out; outer sizes (margins, padding, border). */
static void content_size(wnode_t *n, int *maxw, int *minw){
    *maxw = 0; *minw = 0;
    if(n->text){
        int cwpx = 8 * scale_of(n->st.size);
        int line = 0, word = 0, longest = 0, in_ws = 1;
        for(u32 i = 0; i < n->tlen; i++){
            char c = n->txt[i];
            if(c == ' ' || c == '\t' || c == '\n' || c == '\r'){ if(!in_ws){ line++; in_ws = 1; } word = 0; continue; }
            if((unsigned char)c >= 0x80 && ((unsigned char)c & 0xC0) == 0x80) continue;   /* UTF-8 tail bytes */
            in_ws = 0; line++; word++;
            if(word > longest) longest = word;
        }
        *maxw = line * cwpx;
        *minw = longest * cwpx;
        return;
    }
    const wstyle_t *st = &n->st;
    if(st->display == W_NONE) return;
    int extra = st->p[1] + st->p[3] + st->bl + st->br + st->m[1] + st->m[3];
    if(strcmp(n->tag, "img") == 0){ *maxw = *minw = 8 * 8 * scale_of(st->size); return; }
    if(strcmp(n->tag, "input") == 0){ *maxw = *minw = 20 * 8 * scale_of(st->size); return; }
    int fixed = item_w(st, view_w);
    int run = 0, best = 0, mn = 0;
    int row = st->display == W_FLEX && !st->flex_col;
    int k = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        int cm, cn;
        content_size(c, &cm, &cn);
        if(cn > mn) mn = cn;
        int block = !c->text && c->st.display != W_INLINE && c->st.display != W_NONE;
        if(row){ run += cm + (k++ ? st->gap : 0); continue; }
        if(block){ if(run > best) best = run; run = 0; if(cm > best) best = cm; }
        else run += cm + (c->text ? 8 * scale_of(c->st.size) : 0);    /* a space between inline pieces */
    }
    if(run > best) best = run;
    if(st->display == W_LISTITEM) best += 16 * scale_of(st->size);
    *maxw = (fixed ? fixed : best + extra);
    *minw = (fixed ? (fixed < mn + extra ? fixed : mn + extra) : mn + extra);
}

typedef struct { wnode_t *n; int base, minw, w, grow; } flex_item_t;
#define FLEX_ITEMS 64

/* Lays out one line of flex items from y0; returns the bottom of the tallest. */
static int flex_line(flex_item_t *it, int k, int x0, int w, int gap, int justify, int y0){
    int sum = 0, grow = 0;
    for(int i = 0; i < k; i++){ sum += it[i].w; grow += it[i].grow; }
    int free = w - sum - gap * (k - 1);
    int start = x0, between = gap;
    if(free > 0 && grow > 0){
        int given = 0;
        for(int i = 0; i < k; i++) if(it[i].grow){ int add = free * it[i].grow / grow; it[i].w += add; given += add; }
    } else if(free > 0){
        if(justify == 1) start += free / 2;
        else if(justify == 2) start += free;
        else if(justify == 3 && k > 1) between = gap + free / (k - 1);
        else if(justify == 4){ between = gap + free / k; start += free / k / 2; }
    }
    int x = start, maxy = y0;
    for(int i = 0; i < k; i++){
        wnode_t *c = it[i].n;
        cx = x; cw = it[i].w; cy = y0;
        /* the item fills the width worked out here: its own width is already counted */
        int sw = 0, sp = 0, sv = 0, sm = 0, smp = 0, smv = 0;
        if(!c->text){
            sw = c->st.width; sp = c->st.width_pct; sv = c->st.width_vw;
            sm = c->st.max_w; smp = c->st.max_pct; smv = c->st.max_vw;
            c->st.width = c->st.width_pct = c->st.width_vw = 0;
            c->st.max_w = c->st.max_pct = c->st.max_vw = 0;
        }
        layout_node(c);
        flush_line();
        if(!c->text){
            c->st.width = sw; c->st.width_pct = sp; c->st.width_vw = sv;
            c->st.max_w = sm; c->st.max_pct = smp; c->st.max_vw = smv;
        }
        if(cy > maxy) maxy = cy;
        x += it[i].w + between;
    }
    return maxy;
}

/* A flex row: items take the width their content wants (or their CSS width), grow into
 * free space by flex-grow, are placed by justify-content, and wrap onto more lines when
 * they do not fit (instead of being squeezed into unreadable columns). */
static void flex_children(wnode_t *n, int x0, int w, int gap){
    const wstyle_t *st = &n->st;
    if(st->flex_col){
        /* a column: the items stack like blocks, gap apart */
        int first = 1;
        for(wnode_t *c = n->first; c; c = c->next){
            if(c->text ? blank_text(c) : c->st.display == W_NONE) continue;
            if(!c->text && c->st.display == W_INLINE) c->st.display = W_BLOCK;
            if(!first) cy += gap;
            first = 0;
            cx = x0; cw = w;
            layout_node(c);
            flush_line();
        }
        return;
    }
    flex_item_t mine[FLEX_ITEMS];             /* the GUI runs on the 4 MB boot stack */
    int k = 0;
    for(wnode_t *c = n->first; c && k < FLEX_ITEMS; c = c->next){
        if(c->text ? blank_text(c) : c->st.display == W_NONE) continue;
        if(!c->text && c->st.display == W_INLINE) c->st.display = W_BLOCK;   /* flex items are blocks */
        int mx, mn;
        content_size(c, &mx, &mn);
        int fixed = c->text ? 0 : item_w(&c->st, w);
        int base = fixed ? fixed : mx + 2;
        if(base > w) base = w;
        if(mn > w) mn = w;
        mine[k] = (flex_item_t){ c, base, mn, base, c->text ? 0 : c->st.flex_grow };
        k++;
    }
    if(k == 0) return;

    int y = cy;
    int i = 0;
    while(i < k){
        /* take items while they fit; shrink towards their narrowest when that makes them fit */
        int j = i, sum = 0;
        while(j < k){
            int add = mine[j].base + (j > i ? gap : 0);
            if(j > i && sum + add > w){
                /* would shrinking this line to minimum widths make room? */
                int minsum = 0;
                for(int q = i; q <= j; q++) minsum += mine[q].minw + (q > i ? gap : 0);
                if(st->flex_wrap || minsum > w) break;
            }
            sum += add;
            j++;
        }
        if(sum > w){
            /* shrink each item by its share of the overflow, not below its longest word */
            int over = sum - w, room = 0;
            for(int q = i; q < j; q++) room += mine[q].base - mine[q].minw;
            for(int q = i; q < j; q++){
                int cut = room > 0 ? over * (mine[q].base - mine[q].minw) / room : 0;
                mine[q].w = mine[q].base - cut;
                if(mine[q].w < mine[q].minw) mine[q].w = mine[q].minw;
            }
        } else for(int q = i; q < j; q++) mine[q].w = mine[q].base;
        y = flex_line(&mine[i], j - i, x0, w, gap, st->justify, y);
        if(j < k) y += gap;
        i = j;
    }
    cy = y;
}

/* ---- tables ---- */

#define TABLE_COLS   16
#define TABLE_DEPTH  4
typedef struct { int x, w; int col_w[TABLE_COLS]; int ncols; } table_plan_t;
static table_plan_t table_stack[TABLE_DEPTH];
static int table_depth = 0;

static int cell_span(wnode_t *c){
    const char *v = wnode_attr_get(c, "colspan");
    if(!v) return 1;
    int s = 0;
    while(*v >= '0' && *v <= '9'){ s = s * 10 + (*v - '0'); v++; }
    return s < 1 ? 1 : (s > TABLE_COLS ? TABLE_COLS : s);
}

/* The rows of a table: direct <tr> children and rows inside <thead>/<tbody>/<tfoot>.
 * Fills rows[256] and returns how many there are. */
static int table_collect_rows(wnode_t *n, wnode_t **rows){
    int nr = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        if(c->text) continue;
        if(strcmp(c->tag, "tr") == 0){ if(nr < 256) rows[nr++] = c; continue; }
        if(strcmp(c->tag, "thead") == 0 || strcmp(c->tag, "tbody") == 0 || strcmp(c->tag, "tfoot") == 0){
            for(wnode_t *r = c->first; r; r = r->next)
                if(!r->text && strcmp(r->tag, "tr") == 0 && nr < 256) rows[nr++] = r;
        }
    }
    return nr;
}

/* Works out the column widths from the cells' preferred widths and remembers them. */
static void table_plan(wnode_t *n, int x0, int w){
    if(table_depth >= TABLE_DEPTH) return;
    int need[TABLE_COLS];
    for(int i = 0; i < TABLE_COLS; i++) need[i] = 0;
    int ncols = 0;
    wnode_t *rows[256];
    int nrows = table_collect_rows(n, rows);
    for(int ri = 0; ri < nrows; ri++){
        int col = 0;
        for(wnode_t *c = rows[ri]->first; c; c = c->next){
            if(c->text) continue;
            if(strcmp(c->tag, "td") != 0 && strcmp(c->tag, "th") != 0) continue;
            if(c->st.display == W_NONE){ col += cell_span(c); continue; }
            int span = cell_span(c);
            int mx, mn;
            content_size(c, &mx, &mn);
            if(mx < 20) mx = 20;
            int share = (mx + span - 1) / span;
            for(int k = 0; k < span && col + k < TABLE_COLS; k++)
                if(share > need[col + k]) need[col + k] = share;
            col += span;
        }
        if(col > ncols) ncols = col > TABLE_COLS ? TABLE_COLS : col;
    }
    if(ncols == 0){ ncols = 1; need[0] = w; }
    int sum = 0;
    for(int i = 0; i < ncols; i++) sum += need[i];
    table_plan_t *p = &table_stack[table_depth++];
    p->x = x0; p->w = w; p->ncols = ncols;
    if(sum <= w){
        /* give the extra width equally to the columns */
        int extra = (w - sum) / ncols, over = w - sum - extra * ncols;
        int off = 0;
        for(int i = 0; i < ncols; i++){ p->col_w[i] = need[i] + extra + (i < over ? 1 : 0); off += p->col_w[i]; }
        (void)off;
    } else {
        int used = 0;
        for(int i = 0; i < ncols; i++){ p->col_w[i] = need[i] * w / sum; if(p->col_w[i] < 16) p->col_w[i] = 16; used += p->col_w[i]; }
        /* fix rounding so the table is exactly w wide (trim the last columns) */
        for(int i = ncols - 1; used > w && i >= 0; i--){
            int cut = used - w;
            if(cut > p->col_w[i] - 16) cut = p->col_w[i] - 16;
            if(cut > 0){ p->col_w[i] -= cut; used -= cut; }
        }
    }
}

/* A table row: lays its cells side by side on an equal baseline, like flex_line but 2D. */
static void layout_tr(wnode_t *n){
    wstyle_t *st = &n->st;
    flush_line();
    cy += st->m[0];
    table_plan_t *p = table_depth > 0 ? &table_stack[table_depth - 1] : NULL;
    int row_y = cy;
    int col = 0, x = p ? p->x : cx;
    int max_cy = cy;
    int saved_align = cur_align;
    cur_align = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        if(c->text) continue;
        if(strcmp(c->tag, "td") != 0 && strcmp(c->tag, "th") != 0) continue;
        int span = cell_span(c);
        int wsum = 0, advance = 0;
        if(p){ for(int k = 0; k < span && col + k < p->ncols; k++){ wsum += p->col_w[col + k]; } advance = wsum; }
        else { wsum = cw; advance = cw; }
        int saved_cx = cx, saved_cw = cw;
        cx = x;
        cw = wsum > 0 ? wsum : 40;
        cy = row_y;
        cur_align = 0;
        layout_node(c);                 /* the cell lays itself as a block: padding, border, bg */
        if(cy > max_cy) max_cy = cy;
        cx = saved_cx; cw = saved_cw;
        x += advance;
        if(p) col += span;
    }
    cy = max_cy + st->m[2];
    cur_align = saved_align;
}

/* ---- the tree ---- */

static void layout_node(wnode_t *n){
    wstyle_t *st = &n->st;
    if(n->text){
        cur_dom = n->parent;
        add_text(n->txt, n->tlen, st, cur_link);
        return;
    }
    cur_dom = n;
    if(st->display == W_NONE) return;
    if(strcmp(n->tag, "tr") == 0){ layout_tr(n); return; }
    if(strcmp(n->tag, "br") == 0){ flush_line(); return; }
    if(strcmp(n->tag, "input") == 0){
        /* a text box: its value, or the placeholder in grey, on a dark box */
        char pad[64];
        u32 k = 0;
        const char *shown = n->value && n->value[0] ? n->value : (n->alt ? n->alt : "");
        int has_value = n->value && n->value[0];
        for(const char *q = shown; *q && k < 40; q++) pad[k++] = *q;
        while(k < 20) pad[k++] = ' ';
        pad[k] = 0;
        wstyle_t box = *st;
        box.color = has_value ? st->color : 0x7A8794;
        int saved_set = cur_bg_set; u32 saved_bg = cur_bg;
        cur_bg_set = 1; cur_bg = 0x2A3440;
        cur_dom = n;
        add_piece(pad, (int)k, &box, cur_link);
        cur_bg_set = saved_set; cur_bg = saved_bg;
        return;
    }
    if(strcmp(n->tag, "img") == 0){
        int idx = n->img ? n->img - 1 : -1;
        u32 iw = 0, ih = 0;
        const u32 *px = idx >= 0 ? web_image_pixels(idx, &iw, &ih) : NULL;
        if(px && iw && ih){
            /* width/height attributes win; otherwise the natural size (capped to the container) */
            int dw = 0, dh = 0;
            const char *ws = wnode_attr_get(n, "width"), *hs = wnode_attr_get(n, "height");
            for(const char *q = ws; q && *q >= '0' && *q <= '9'; q++) dw = dw * 10 + (*q - '0');
            for(const char *q = hs; q && *q >= '0' && *q <= '9'; q++) dh = dh * 10 + (*q - '0');
            if(dw <= 0) dw = (int)iw;
            if(dh <= 0) dh = (int)((u64)dw * ih / iw);
            if(dh <= 0) dh = 1;
            if(dw > cw){ dh = (int)((u64)dh * cw / (dw ? dw : 1)); dw = cw; }
            flush_line();
            cy += st->m[0];
            if(ncmds < CMD_MAX){
                web_cmd_t im = { .kind = WEB_CMD_IMAGE, .x = cx + st->m[3], .y = cy, .w = dw, .h = dh,
                                 .color = 0, .link = cur_link, .dom = n, .img = idx };
                emit(&im);
            }
            cy += dh;
            cy += st->m[2];
            return;
        }
        /* not decoded (yet): the alt text */
        char alt[120] = "[image";
        if(n->alt && n->alt[0]){ int k = (int)strlen(alt); alt[k++] = ':'; alt[k++] = ' '; for(const char *a = n->alt; *a && k < 110; a++) alt[k++] = *a; alt[k] = 0; }
        int k = (int)strlen(alt);
        alt[k++] = ']'; alt[k] = 0;
        wstyle_t muted = *st;
        muted.color = 0x9AA7B0;
        add_text(alt, (u32)k, &muted, cur_link);
        return;
    }
    if(strcmp(n->tag, "hr") == 0){
        flush_line();
        cy += st->m[0];
        web_cmd_t r = { .kind = WEB_CMD_RECT, .x = cx + st->m[3], .y = cy, .w = cw - st->m[1] - st->m[3],
                        .h = 2, .color = 0x3A4450, .link = -1 };
        emit(&r);
        cy += 2 + st->m[2];
        return;
    }

    int saved_link = cur_link;
    if(strcmp(n->tag, "a") == 0 && n->href && web_nlinks < WEB_LINKS_MAX){
        web_link_list[web_nlinks] = strdup(n->href);
        if(web_link_list[web_nlinks]) cur_link = web_nlinks;
        web_nlinks++;
    }

    if(st->display == W_INLINE){
        /* an inline box's background covers the words inside it */
        int saved_bg_set = cur_bg_set;
        u32 saved_bg = cur_bg;
        if(st->bg_set){ cur_bg_set = 1; cur_bg = st->bg; }
        for(wnode_t *c = n->first; c; c = c->next) layout_node(c);
        cur_bg_set = saved_bg_set;
        cur_bg = saved_bg;
        cur_link = saved_link;
        return;
    }

    /* block (or list item) */
    flush_line();
    cy += st->m[0];
    int saved_cx = cx, saved_cw = cw, saved_align = cur_align;
    int box_top = cy;
    int bt = st->bt, br = st->br, bb = st->bb, bl = st->bl;
    int bw = bl + br;
    /* the content width: width (px, % of the container, vw) capped by max-width */
    int avail = cw - st->m[1] - st->m[3];
    int extra = st->p[1] + st->p[3] + bw;
    int content = avail - extra;
    int want = st->width + cw * st->width_pct / 1000 + view_w * st->width_vw / 1000;
    if(want > 0){ content = st->border_box ? want - extra : want; }
    int maxw = st->max_w + cw * st->max_pct / 1000 + view_w * st->max_vw / 1000;
    if(maxw > 0){ int m = st->border_box ? maxw - extra : maxw; if(content > m) content = m; }
    if(content < 40) content = 40;
    int box_w = content + extra;
    int box_x = cx + st->m[3];
    if(box_w < avail){
        if(st->auto_lr == 3) box_x += (avail - box_w) / 2;            /* margin: 0 auto */
        else if(st->auto_lr == 2) box_x += avail - box_w;
    }
    cy += st->p[0] + bt;
    cx = box_x + st->p[3] + bl;
    cw = content;
    cur_align = st->align;
    if(st->display == W_LISTITEM && !st->no_bullet){
        wstyle_t bullet = *st;
        bullet.color = 0x9AA7B0;
        if(n->parent && strcmp(n->parent->tag, "ol") == 0){
            /* ordered list: a number, not a disc */
            int num = 1;
            for(wnode_t *sib = n->parent->first; sib && sib != n; sib = sib->next)
                if(!sib->text && strcmp(sib->tag, "li") == 0) num++;
            char buf[16]; int k = 0; char tmp[12]; int t = 0;
            if(num <= 0) tmp[t++] = '0';
            for(int v = num; v > 0; v /= 10) tmp[t++] = (char)('0' + v % 10);
            while(t > 0) buf[k++] = tmp[--t];
            buf[k++] = '.';
            add_text(buf, (u32)k, &bullet, -1);
        } else {
            add_text("\x07", 1, &bullet, -1);     /* a centred filled disc, not an asterisk */
        }
        pending_space = 1;
    }
    int is_table = strcmp(n->tag, "table") == 0 && table_depth < TABLE_DEPTH;
    if(is_table) table_plan(n, cx, cw);
    if(st->display == W_FLEX){
        flex_children(n, cx, cw, st->gap);
    } else {
        for(wnode_t *c = n->first; c; c = c->next) layout_node(c);
    }
    if(is_table) table_depth--;
    flush_line();
    if(st->height > 0){
        int content_h = cy - (box_top + st->m[0] + st->p[0] + bt);
        if(st->height > content_h) cy += st->height - content_h;
    }
    cy += st->p[2] + bb;
    int box_h = cy - box_top;
    if(st->bg_set && ncmds < CMD_MAX){
        web_cmd_t bg = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top, .w = box_w, .h = box_h,
                         .color = st->bg, .link = -1 };
        /* the background goes behind what was already drawn inside the box */
        for(int i = ncmds; i > 0; i--) cmds[i] = cmds[i - 1];
        cmds[0] = bg;
        ncmds++;
    }
    if(bt > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top, .w = box_w, .h = bt, .color = st->border_color, .link = -1 };
        emit(&e);
    }
    if(bb > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top + box_h - bb, .w = box_w, .h = bb, .color = st->border_color, .link = -1 };
        emit(&e);
    }
    if(bl > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top, .w = bl, .h = box_h, .color = st->border_color, .link = -1 };
        emit(&e);
    }
    if(br > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x + box_w - br, .y = box_top, .w = br, .h = box_h, .color = st->border_color, .link = -1 };
        emit(&e);
    }
    cy += st->m[2];
    cx = saved_cx; cw = saved_cw; cur_align = saved_align;
    cur_link = saved_link;
}

/* Computes every element's style, top-down. */
static void style_tree(wnode_t *n, wnode_t *parent){
    css_apply(n, parent);
    for(wnode_t *c = n->first; c; c = c->next) style_tree(c, n);
}

int web_layout(int width){
    if(!web_root) return 0;
    if(!cmds) cmds = kmalloc(sizeof(web_cmd_t) * CMD_MAX);
    if(!arena) arena = kmalloc(ARENA_MAX);
    if(!cmds || !arena) return 0;

    for(int i = 0; i < web_nlinks; i++){ if(web_link_list[i]) kfree(web_link_list[i]); web_link_list[i] = NULL; }
    web_nlinks = 0;

    css_reset();
    css_set_viewport(width);
    if(web_css_ext) css_add_sheet(web_css_ext, web_css_ext_len);    /* linked sheets come first */
    if(web_css) css_add_sheet(web_css, web_css_len);
    style_tree(web_root, NULL);

    ncmds = 0;
    arena_n = 0;
    nfrag = 0;
    used = 0;
    pending_space = 0;
    cur_align = 0;
    cur_link = -1;
    view_w = width;
    cx = 8; cw = width - 16; cy = 8;
    if(cw < 40) cw = 40;
    layout_node(web_root);
    flush_line();
    return cy + 8;
}

const web_cmd_t *web_cmds(int *n){
    if(n) *n = ncmds;
    return cmds;
}

/* Frees the layout buffers (made again by the next web_layout). */
void web_layout_release(void){
    if(cmds) kfree(cmds);
    if(arena) kfree(arena);
    cmds = NULL;
    arena = NULL;
    ncmds = 0;
    arena_n = 0;
}
