/* Layout: walks the styled tree and produces drawing commands. Block boxes stack
 * vertically; inline words and images flow left to right and wrap at the box width.
 * Floats and inline-blocks sit side by side in rows, flex and grid containers place
 * their items in lines and tracks, tables share their width out by column.
 *
 * Lengths are device pixels (see WEB_DPR_NUM in web_internal.h). */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "string.h"

#define CMD_LIMIT    400000          /* drawing commands, at most */
#define ACHUNK       (256 * 1024)    /* text is copied into chunks of this size */
#define ACHUNKS      64
#define FRAG_MAX     512

static web_cmd_t *cmds = NULL;
static int ncmds = 0, cmd_cap = 0;
static u8 *cmd_top = NULL;           /* 1: drawn by a positioned box, painted over the rest */
static int top_depth = 0;            /* inside a positioned box being laid out */
static char *achunk[ACHUNKS];
static int acur = 0;
static u32 aused = 0;
static u32 layout_gen = 1;           /* content_size() caches are valid for one pass */

typedef struct {
    const void *dom;
    int bg_set;
    u32 bg;
    int x, w, h;              /* x within the line; width; height (boxes: images) */
    int scale;                /* text: glyph scale; 0 for a box */
    u32 color;
    u8 bold, italic, underline, strike;
    int link;
    const char *text;         /* copy in the arena */
    int img;                  /* a box: the image index, or -1 for an empty box */
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
static const void *cur_dom = NULL;

/* ---- output ---- */

static int grow_cmds(void){
    if(cmd_cap >= CMD_LIMIT) return 0;
    int nc = cmd_cap ? cmd_cap * 2 : 8192;
    if(nc > CMD_LIMIT) nc = CMD_LIMIT;
    web_cmd_t *g = kmalloc(sizeof(web_cmd_t) * (u32)nc);
    u8 *t = kmalloc((u32)nc);
    if(!g || !t){ if(g) kfree(g); if(t) kfree(t); return 0; }
    if(cmds){ memcpy(g, cmds, sizeof(web_cmd_t) * (u32)ncmds); kfree(cmds); }
    if(cmd_top){ memcpy(t, cmd_top, (u32)ncmds); kfree(cmd_top); }
    cmds = g; cmd_cap = nc; cmd_top = t;
    return 1;
}
static int emit(const web_cmd_t *c){
    if(ncmds == cmd_cap && !grow_cmds()) return -1;
    cmds[ncmds] = *c;
    cmd_top[ncmds] = top_depth > 0;
    return ncmds++;
}
/* A slot for a box's background, filled in once the box's height is known: it is drawn
 * before (under) everything inside the box. */
static int bg_slot(void){
    web_cmd_t r = { .kind = WEB_CMD_RECT, .w = 0, .h = 0, .link = -1, .img = -1 };
    return emit(&r);
}
/* A slot for a box's background picture (drawn over its colour, under its content). */
static int bgimg_slot(void){
    web_cmd_t r = { .kind = WEB_CMD_BGIMG, .w = 0, .h = 0, .link = -1, .img = -1 };
    return emit(&r);
}

/* Works out where the picture goes in the box (x, y, w, h: the padding box) and what is drawn. */
static void bgimg_fill(int slot, const wstyle_t *st, int idx, int x, int y, int w, int h){
    if(slot < 0 || slot >= ncmds) return;
    u32 nw = 0, nh = 0;
    if(!web_image_natural(idx, &nw, &nh) || !nw || !nh || w <= 0 || h <= 0){ cmds[slot].w = 0; cmds[slot].h = 0; return; }
    int iw = CSSPX((int)nw), ih = CSSPX((int)nh);
    int dw = iw, dh = ih;
    if(st->bg_size_mode == 1 || st->bg_size_mode == 2){
        long long sw = (long long)w * 1000 / iw, sh = (long long)h * 1000 / ih;
        long long k = st->bg_size_mode == 1 ? (sw > sh ? sw : sh) : (sw < sh ? sw : sh);
        dw = (int)((long long)iw * k / 1000); dh = (int)((long long)ih * k / 1000);
    } else if(st->bg_size_mode == 3){
        int ew = st->bg_w >= 0 ? st->bg_w + (int)((long long)w * st->bg_wp / 1000) : -1;
        int eh = st->bg_h >= 0 ? st->bg_h + (int)((long long)h * st->bg_hp / 1000) : -1;
        if(ew >= 0 && eh >= 0){ dw = ew; dh = eh; }
        else if(ew >= 0){ dw = ew; dh = (int)((long long)ih * ew / iw); }
        else if(eh >= 0){ dh = eh; dw = (int)((long long)iw * eh / ih); }
    }
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
    int ox = (int)((long long)(w - dw) * st->bg_xp / 1000) + st->bg_x;
    int oy = (int)((long long)(h - dh) * st->bg_yp / 1000) + st->bg_y;
    cmds[slot].x = x; cmds[slot].y = y; cmds[slot].w = w; cmds[slot].h = h;
    cmds[slot].img = idx; cmds[slot].scale = dw; cmds[slot].bold = dh; cmds[slot].italic = st->bg_repeat;
    cmds[slot].ox = ox; cmds[slot].oy = oy;
}

static void bg_fill(int slot, int x, int y, int w, int h, u32 color){
    if(slot < 0 || slot >= ncmds) return;
    cmds[slot].x = x; cmds[slot].y = y; cmds[slot].w = w; cmds[slot].h = h; cmds[slot].color = color;
}

static const char *arena_put(const char *s, int n){
    if(n < 0) return NULL;
    if((u32)n + 1 > ACHUNK) n = ACHUNK - 1;
    if(!achunk[acur] || aused + (u32)n + 1 > ACHUNK){
        if(achunk[acur]){ if(acur + 1 >= ACHUNKS) return NULL; acur++; aused = 0; }
        if(!achunk[acur]){ achunk[acur] = kmalloc(ACHUNK); if(!achunk[acur]) return NULL; }
    }
    char *d = achunk[acur] + aused;
    memcpy(d, s, (u32)n);
    d[n] = 0;
    aused += (u32)n + 1;
    return d;
}

/* The glyph size for a font size (px): in quarter steps of the 8 x 16 font, so a glyph is
 * 2*s wide and 4*s tall. Never below the font's own size. */
static int scale_of(int size){
    int s4 = (size + 2) / 4;
    return s4 < 4 ? 4 : s4 > 64 ? 64 : s4;
}
#define GLYPH_W(s4) (2 * (s4))
#define GLYPH_H(s4) (4 * (s4))

static void flush_line(void){
    if(nfrag == 0) return;
    int lh = 0, text_h = 0;
    for(int i = 0; i < nfrag; i++){
        int h = frags[i].scale ? GLYPH_H(frags[i].scale) : frags[i].h;
        if(frags[i].scale && h > text_h) text_h = h;
        if(h > lh) lh = h;
    }
    if(lh < 1) lh = 1;
    int offset = 0;
    if(cur_align == 1) offset = (cw - used) / 2;
    else if(cur_align == 2) offset = cw - used;
    if(offset < 0) offset = 0;
    for(int i = 0; i < nfrag; i++){
        frag_t *f = &frags[i];
        int fh = f->scale ? GLYPH_H(f->scale) : f->h;
        int top = cy + (lh - fh);
        int x = cx + offset + f->x;
        if(f->bg_set){
            /* an inline box's background sits behind its words */
            web_cmd_t b = { .kind = WEB_CMD_RECT, .x = x, .y = top, .w = f->w, .h = fh, .color = f->bg, .link = f->link, .dom = f->dom, .img = -1 };
            emit(&b);
        }
        if(!f->scale){
            if(f->img >= 0){
                web_cmd_t im = { .kind = WEB_CMD_IMAGE, .x = x, .y = top, .w = f->w, .h = f->h, .link = f->link, .dom = f->dom, .img = f->img };
                emit(&im);
            }
            continue;
        }
        web_cmd_t c = { .kind = WEB_CMD_TEXT, .x = x, .y = top, .w = f->w, .h = fh,
                        .color = f->color, .scale = f->scale, .bold = f->bold, .italic = f->italic, .text = f->text, .link = f->link,
                        .dom = f->dom, .img = -1 };
        emit(&c);
        if(f->underline){
            web_cmd_t u = { .kind = WEB_CMD_RECT, .x = x, .y = top + fh - f->scale / 4 - 2, .w = f->w, .h = f->scale >= 7 ? 2 : 1,
                            .color = f->color, .link = -1, .img = -1 };
            emit(&u);
        }
        if(f->strike){
            web_cmd_t u = { .kind = WEB_CMD_RECT, .x = x, .y = top + fh / 2, .w = f->w, .h = f->scale >= 7 ? 2 : 1,
                            .color = f->color, .link = -1, .img = -1 };
            emit(&u);
        }
    }
    cy += lh + (text_h ? text_h / 5 : 0);
    nfrag = 0;
    used = 0;
    pending_space = 0;
}

/* ---- words and boxes on a line ---- */

static void add_piece(const char *w, int wl, wstyle_t *st, int link){
    int sc = scale_of(st->size);
    int cwpx = GLYPH_W(sc);
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
        frags[nfrag++] = (frag_t){ cur_dom, cur_bg_set, cur_bg, used, fit * cwpx, 0, sc, st->color, (u8)st->bold, (u8)st->italic, (u8)st->underline, st->strike, link, t, -1 };
        used += fit * cwpx;
        flush_line();
        w += fit; wl -= fit;
    }
    const char *t = arena_put(w, wl);
    if(!t) return;
    frags[nfrag++] = (frag_t){ cur_dom, cur_bg_set, cur_bg, used + space, wl * cwpx, 0, sc, st->color, (u8)st->bold, (u8)st->italic, (u8)st->underline, st->strike, link, t, -1 };
    used += space + wl * cwpx;
    pending_space = 0;
}

/* An image (or a reserved empty box) on the line, like a big letter. */
static void add_box(int w, int h, int img, int link, const void *dom, int mleft, int mright){
    int space = (pending_space && nfrag > 0) ? CSSPX(4) : 0;
    int need = space + mleft + w + mright;
    if(nfrag > 0 && used + need > cw){ flush_line(); space = 0; need = mleft + w + mright; }
    if(nfrag == FRAG_MAX) flush_line();
    frags[nfrag++] = (frag_t){ dom, 0, 0, used + space + mleft, w, h, 0, 0, 0, 0, 0, 0, link, NULL, img };
    used += need;
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
    if(cp >= 0x100 && cp <= 0x17F){
        /* Latin Extended-A: the base letter */
        static const char ext[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        one[0] = ext[cp - 0x100]; one[1] = 0;
        return one;
    }
    switch(cp){
        case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: case 0x2007: case 0x2008: case 0x3000: return " ";
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: case 0xFE0F: case 0x200E: case 0x200F: case 0x2060: return "";
        case 0xA9: return "(c)";   case 0xAE: return "(R)";   case 0x2122: return "TM";
        case 0xB0: return "o";     case 0xB7: case 0x2022: case 0x2023: case 0x25CF: case 0x2219: case 0x22C5: case 0x2027: return "*";
        case 0xAB: return "<<";    case 0xBB: return ">>";    case 0xD7: return "x";    case 0xF7: return "/";
        case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x02BC: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: case 0x2E3A: return "-";
        case 0x2026: return "...";
        case 0x2039: return "<";   case 0x203A: return ">";
        case 0x20AC: return "EUR"; case 0xA3: return "GBP";   case 0xA5: return "JPY";  case 0xA2: return "c";  case 0x20B9: return "Rs";
        case 0x2190: return "<-";  case 0x2192: return "->";  case 0x2191: return "^";  case 0x2193: return "v";
        case 0x21D2: return "=>";  case 0x2194: return "<->"; case 0x21D0: return "<=";  case 0x21D4: return "<=>";
        case 0x25B2: case 0x25B4: case 0x2303: return "^";  case 0x25BC: case 0x25BE: case 0x2304: return "v";
        case 0x25B6: case 0x25B8: case 0x276F: return ">";  case 0x25C0: case 0x25C2: case 0x276E: return "<";
        case 0x2713: case 0x2714: return "v";  case 0x2717: case 0x2718: case 0x2715: case 0x2716: return "x";
        case 0x2605: case 0x2606: return "*";
        case 0xBC: return "1/4";   case 0xBD: return "1/2";   case 0xBE: return "3/4";
        case 0xB1: return "+/-";   case 0x2264: return "<=";  case 0x2265: return ">=";  case 0x2260: return "!=";
        case 0x2261: return "=";   case 0x2248: return "~";   case 0x221E: return "inf"; case 0x2208: return "in";
        case 0x2211: return "S";   case 0x220F: return "P";   case 0x221A: return "sqrt"; case 0x2202: return "d";
        case 0x00B9: return "1";   case 0x00B2: return "2";   case 0x00B3: return "3";
        case 0xA7: return "S";     case 0xB6: return "P";     case 0xBF: return "?";    case 0xA1: return "!";
        case 0x2020: return "+";   case 0x2021: return "++";  case 0x2030: return "%o";
        case 0x2116: return "No";  case 0x2103: return "C";   case 0x00B5: case 0x03BC: return "u";
        case 0x0391: case 0x03B1: return "a"; case 0x0392: case 0x03B2: return "b"; case 0x0393: case 0x03B3: return "g";
        case 0x0394: case 0x03B4: return "d"; case 0x03A0: case 0x03C0: return "pi"; case 0x03A3: case 0x03C3: return "s";
        case 0x03A9: case 0x03C9: return "w"; case 0x03BB: return "l"; case 0x03B8: return "th"; case 0x03C6: return "ph";
        case 0x2630: return "=";
    }
    if(cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xE000 && cp <= 0xF8FF)) return "";   /* emoji, symbols, icon fonts */
    if(cp >= 0x0300 && cp <= 0x036F) return "";                     /* combining accents */
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
        { "sbquo;", 0x201A }, { "bdquo;", 0x201E }, { "prime;", 0x2032 }, { "Prime;", 0x2033 },
        { "eacute;", 0xE9 }, { "egrave;", 0xE8 }, { "ecirc;", 0xEA }, { "euml;", 0xEB }, { "agrave;", 0xE0 },
        { "aacute;", 0xE1 }, { "acirc;", 0xE2 }, { "atilde;", 0xE3 }, { "auml;", 0xE4 }, { "aring;", 0xE5 },
        { "aelig;", 0xE6 }, { "ccedil;", 0xE7 }, { "igrave;", 0xEC }, { "iacute;", 0xED }, { "icirc;", 0xEE },
        { "iuml;", 0xEF }, { "ntilde;", 0xF1 }, { "ograve;", 0xF2 }, { "oacute;", 0xF3 }, { "ocirc;", 0xF4 },
        { "otilde;", 0xF5 }, { "ouml;", 0xF6 }, { "oslash;", 0xF8 }, { "ugrave;", 0xF9 }, { "uacute;", 0xFA },
        { "ucirc;", 0xFB }, { "uuml;", 0xFC }, { "yacute;", 0xFD }, { "yuml;", 0xFF }, { "szlig;", 0xDF },
        { "Eacute;", 0xC9 }, { "Egrave;", 0xC8 }, { "Agrave;", 0xC0 }, { "Aacute;", 0xC1 }, { "Auml;", 0xC4 },
        { "Ouml;", 0xD6 }, { "Uuml;", 0xDC }, { "Ccedil;", 0xC7 }, { "Ntilde;", 0xD1 }, { "Aring;", 0xC5 },
        { "hellip;", 0x2026 }, { "bull;", 0x2022 }, { "middot;", 0xB7 }, { "raquo;", 0xBB }, { "laquo;", 0xAB },
        { "rsaquo;", 0x203A }, { "lsaquo;", 0x2039 }, { "times;", 0xD7 }, { "divide;", 0xF7 },
        { "rarr;", 0x2192 }, { "larr;", 0x2190 }, { "uarr;", 0x2191 }, { "darr;", 0x2193 }, { "harr;", 0x2194 },
        { "rArr;", 0x21D2 }, { "lArr;", 0x21D0 }, { "hArr;", 0x21D4 }, { "trade;", 0x2122 },
        { "euro;", 0x20AC }, { "pound;", 0xA3 }, { "yen;", 0xA5 }, { "cent;", 0xA2 }, { "curren;", 0xA4 },
        { "shy;", 0xAD }, { "zwj;", 0x200D }, { "zwnj;", 0x200C }, { "thinsp;", 0x2009 }, { "ensp;", 0x2002 }, { "emsp;", 0x2003 },
        { "sect;", 0xA7 }, { "para;", 0xB6 }, { "iexcl;", 0xA1 }, { "iquest;", 0xBF }, { "plusmn;", 0xB1 },
        { "frac12;", 0xBD }, { "frac14;", 0xBC }, { "frac34;", 0xBE }, { "sup1;", 0xB9 }, { "sup2;", 0xB2 }, { "sup3;", 0xB3 },
        { "micro;", 0xB5 }, { "le;", 0x2264 }, { "ge;", 0x2265 }, { "ne;", 0x2260 }, { "equiv;", 0x2261 },
        { "asymp;", 0x2248 }, { "infin;", 0x221E }, { "minus;", 0x2212 }, { "sum;", 0x2211 }, { "prod;", 0x220F },
        { "radic;", 0x221A }, { "part;", 0x2202 }, { "isin;", 0x2208 }, { "dagger;", 0x2020 }, { "Dagger;", 0x2021 },
        { "permil;", 0x2030 }, { "alpha;", 0x3B1 }, { "beta;", 0x3B2 }, { "gamma;", 0x3B3 }, { "delta;", 0x3B4 },
        { "pi;", 0x3C0 }, { "sigma;", 0x3C3 }, { "omega;", 0x3C9 }, { "lambda;", 0x3BB }, { "mu;", 0x3BC },
        { "theta;", 0x3B8 }, { "phi;", 0x3C6 }, { "check;", 0x2713 }, { "star;", 0x2606 }, { "starf;", 0x2605 },
        { "hearts;", 0x2665 }, { "lrm;", 0x200E }, { "rlm;", 0x200F }, { "ordm;", 0xBA }, { "ordf;", 0xAA },
        { "macr;", 0xAF }, { "acute;", 0xB4 }, { "cedil;", 0xB8 }, { "uml;", 0xA8 }, { "not;", 0xAC }, { "brvbar;", 0xA6 },
        { "NewLine;", '\n' }, { "Tab;", '\t' }, { "colon;", ':' }, { "comma;", ',' }, { "period;", '.' },
        { "excl;", '!' }, { "quest;", '?' }, { "num;", '#' }, { "dollar;", '$' }, { "percnt;", '%' },
        { "lpar;", '(' }, { "rpar;", ')' }, { "ast;", '*' }, { "plus;", '+' }, { "sol;", '/' }, { "bsol;", '\\' },
        { "lowbar;", '_' }, { "verbar;", '|' }, { "vert;", '|' }, { "lbrack;", '[' }, { "rbrack;", ']' },
        { "lcub;", '{' }, { "rcub;", '}' }, { "semi;", ';' }, { "equals;", '=' }, { "commat;", '@' }, { "Hat;", '^' },
        { "grave;", '`' }, { "tilde;", '~' },
        /* no semicolon (old pages) */
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE },
    };
    if(n < 2) return 0;
    char c1 = s[1];
    for(u32 i = 0; i < sizeof e / sizeof e[0]; i++){
        if(e[i].name[0] != c1) continue;
        u32 k = 0, nl = (u32)strlen(e[i].name);
        if(nl > n - 1) continue;
        while(k < nl && s[1 + k] == e[i].name[k]) k++;
        if(k == nl){
            /* a bare "&amp" only counts when no letter follows (so "&ampere" stays) */
            if(e[i].name[nl - 1] != ';' && 1 + nl < n){ char f = s[1 + nl]; if((f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z') || (f >= '0' && f <= '9') || f == '=') continue; }
            *out = web_uni_ascii(e[i].cp);
            if(e[i].cp == '\n'){ *out = " "; }
            return (int)nl + 1;
        }
    }
    if(n > 2 && s[1] == '#'){
        u32 v = 0, k = 2;
        int hex = k < n && (s[k] == 'x' || s[k] == 'X');
        if(hex) k++;
        u32 d0 = k;
        for(; k < n && k < d0 + 8; k++){
            char c = s[k];
            if(c >= '0' && c <= '9') v = v * (hex ? 16 : 10) + (u32)(c - '0');
            else if(hex && c >= 'a' && c <= 'f') v = v * 16 + (u32)(c - 'a' + 10);
            else if(hex && c >= 'A' && c <= 'F') v = v * 16 + (u32)(c - 'A' + 10);
            else break;
        }
        if(k > d0){
            /* Windows-1252 codes that pages write as &#150; and the like */
            if(v >= 0x80 && v <= 0x9F){
                static const u16 w1252[32] = { 0x20AC, 0, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17D, 0,
                                               0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0, 0x17E, 0x178 };
                if(w1252[v - 0x80]) v = w1252[v - 0x80];
            }
            *out = web_uni_ascii(v);
            if(v == 10 || v == 13 || v == 9) *out = " ";
            return (int)(k < n && s[k] == ';' ? k + 1 : k);
        }
    }
    return 0;
}

static void case_word(char *w, int wl, int upper){
    for(int i = 0; i < wl; i++){
        if(upper == 1 && w[i] >= 'a' && w[i] <= 'z') w[i] = (char)(w[i] - 32);
        else if(upper == 2 && w[i] >= 'A' && w[i] <= 'Z') w[i] = (char)(w[i] + 32);
    }
}

/* Splits text into words (collapsing white space) and adds them. */
static void add_text(const char *s, u32 n, wstyle_t *st, int link){
    if(st->hide_text) return;
    if(st->white_pre){
        /* keep spaces and newlines: emit each line as pieces, break on '\n' */
        char buf[160]; int bl = 0;
        u32 i = 0;
        if(n > 0 && s[0] == '\n') i = 1;             /* a single leading newline after <pre> is dropped */
        for(; i < n; ){
            char c = s[i];
            if(c == '\r'){ i++; continue; }
            if(c == '\n'){ if(bl){ case_word(buf, bl, st->upper); add_piece(buf, bl, st, link); bl = 0; } flush_line(); pending_space = 0; i++; continue; }
            if(c == '\t'){ i++; for(int k = 0; k < 4 && bl < 159; k++) buf[bl++] = ' '; }
            else if(c == '&'){
                const char *rep = NULL; int used_n = web_entity(s + i, n - i, &rep);
                if(used_n) i += (u32)used_n; else { rep = "&"; i++; }
                for(; rep && *rep; rep++) if(bl < 159) buf[bl++] = *rep;
            } else if((unsigned char)c >= 0x80){
                u32 cp; i += (u32)web_utf8(s + i, n - i, &cp);
                const char *rep = web_uni_ascii(cp);
                for(; rep && *rep; rep++) if(bl < 159) buf[bl++] = *rep;
            } else { if(bl < 159) buf[bl++] = c; i++; }
            if(bl >= 150){ case_word(buf, bl, st->upper); add_piece(buf, bl, st, link); bl = 0; }   /* long lines in chunks */
        }
        if(bl){ case_word(buf, bl, st->upper); add_piece(buf, bl, st, link); }
        return;
    }
    char word[160];
    int wl = 0;
    for(u32 i = 0; i < n; ){
        char c = s[i];
        int is_ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
        if(is_ws){
            if(wl){ case_word(word, wl, st->upper); add_piece(word, wl, st, link); wl = 0; }
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
    if(wl){ case_word(word, wl, st->upper); add_piece(word, wl, st, link); }
}

static void layout_node(wnode_t *n);

/* ---- position: absolute ----
 * An absolutely positioned box leaves the flow: it is queued where it appears and laid out
 * when its containing block (the nearest positioned ancestor, or the page) is finished, at
 * its top/left/right/bottom, over what is already there. */
typedef struct { wnode_t *n; int sx, sy, link; } absq_t;
#define ABS_MAX 1024
static absq_t absq[ABS_MAX];
static int nabs = 0;
static wnode_t *abs_now = NULL;      /* the queued box being laid out now (not queued again) */

static int out_of_flow(const wnode_t *c){
    return !c->text && c->st.pos == 2 && c->st.display != W_NONE;
}

static int blank_text(const wnode_t *c){
    for(u32 i = 0; i < c->tlen; i++){
        char ch = c->txt[i];
        if(ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return 0;
    }
    return 1;
}
static int blank(const wnode_t *c){ return c->text ? blank_text(c) : (c->st.display == W_NONE || c->st.pos == 2); }

/* A box's outer width when its CSS gives one (0 = none given), within w. */
static int item_w(const wstyle_t *st, int w){
    int want = st->width + (int)((long long)w * st->width_pct / 1000) + view_w * st->width_vw / 1000;
    if(want <= 0) return 0;
    if(st->min_w > want) want = st->min_w;
    int extra = st->border_box ? 0 : st->p[1] + st->p[3] + st->bl + st->br;
    int out = want + extra + st->m[1] + st->m[3];
    return out > w ? w : out;
}

/* The size an <img> is drawn at within a box w wide: *dw x *dh. 0 if unknown yet. */
static int img_size(wnode_t *n, int w, int *dw, int *dh){
    const wstyle_t *st = &n->st;
    int idx = n->img ? n->img - 1 : -1;
    u32 iw = 0, ih = 0;
    const u32 *px = idx >= 0 ? web_image_pixels(idx, &iw, &ih) : NULL;
    if(px) web_image_natural(idx, &iw, &ih);                      /* an SVG's own size, not its sharper raster's */
    int cw_ = st->width + (int)((long long)w * st->width_pct / 1000) + view_w * st->width_vw / 1000;
    int ch_ = st->height;
    if(cw_ > 0 && st->border_box) cw_ -= st->p[1] + st->p[3] + st->bl + st->br;
    if(px && iw && ih){
        if(cw_ > 0 && ch_ > 0){ *dw = cw_; *dh = ch_; }
        else if(cw_ > 0){ *dw = cw_; *dh = (int)((u64)cw_ * ih / iw); }
        else if(ch_ > 0){ *dh = ch_; *dw = (int)((u64)ch_ * iw / ih); }
        else { *dw = CSSPX((int)iw); *dh = CSSPX((int)ih); }
    } else {
        if(cw_ <= 0 || ch_ <= 0) return 0;
        *dw = cw_; *dh = ch_;
    }
    int maxw = st->max_w + (int)((long long)w * st->max_pct / 1000) + view_w * st->max_vw / 1000;
    if(maxw > 0 && *dw > maxw){ *dh = (int)((long long)*dh * maxw / *dw); *dw = maxw; }
    if(*dw > w && w > 0){ *dh = (int)((long long)*dh * w / *dw); *dw = w; }
    if(*dw < 1) *dw = 1;
    if(*dh < 1) *dh = 1;
    return 1;
}

/* A <video>'s box: CSS width/height, else the width/height attributes, else 640x360;
 * one of them alone keeps 16:9. Never wider than w. */
static int attr_px(wnode_t *n, const char *name){
    const char *v = wnode_attr_get(n, name);
    int x = 0;
    if(!v) return 0;
    while(*v >= '0' && *v <= '9'){ x = x * 10 + (*v - '0'); v++; }
    return x > 0 ? CSSPX(x) : 0;
}
static void video_size(wnode_t *n, int w, int *dw, int *dh){
    const wstyle_t *st = &n->st;
    int cw_ = st->width + (int)((long long)w * st->width_pct / 1000) + view_w * st->width_vw / 1000;
    int ch_ = st->height;
    if(cw_ <= 0) cw_ = attr_px(n, "width");
    if(ch_ <= 0) ch_ = attr_px(n, "height");
    if(cw_ <= 0 && ch_ <= 0){ cw_ = CSSPX(640); ch_ = CSSPX(360); }
    else if(cw_ <= 0) cw_ = ch_ * 16 / 9;
    else if(ch_ <= 0) ch_ = cw_ * 9 / 16;
    if(w > 0 && cw_ > w){ ch_ = (int)((long long)ch_ * w / cw_); cw_ = w; }
    *dw = cw_ < 1 ? 1 : cw_;
    *dh = ch_ < 1 ? 1 : ch_;
}

/* the <video> box from the last layout (page coordinates) */
int web_video_box(int *x, int *y, int *w, int *h){
    int n = 0;
    const web_cmd_t *c = web_cmds(&n);
    for(int i = 0; c && i < n; i++) if(c[i].kind == WEB_CMD_VIDEO){ *x = c[i].x; *y = c[i].y; *w = c[i].w; *h = c[i].h; return 1; }
    return 0;
}

/* How wide a node wants to be: on one line (max) and at its narrowest, the longest word (min).
 * An estimate from the text, without laying it out; outer sizes (margins, padding, border). */
static void content_size(wnode_t *n, int *maxw, int *minw){
    *maxw = 0; *minw = 0;
    if(n->cs_gen == layout_gen){ *maxw = n->cs_max; *minw = n->cs_min; return; }
    if(n->text){
        if(n->st.hide_text){ n->cs_gen = layout_gen; n->cs_max = n->cs_min = 0; return; }
        int cwpx = GLYPH_W(scale_of(n->st.size));
        if(n->st.white_pre){
            /* preformatted: the longest line, spaces and all (it is not wrapped) */
            int col = 0, widest = 0;
            for(u32 i = 0; i < n->tlen; i++){
                char c = n->txt[i];
                if(c == '\n'){ col = 0; continue; }
                if(c == '\r') continue;
                if((unsigned char)c >= 0x80 && ((unsigned char)c & 0xC0) == 0x80) continue;
                col += c == '\t' ? 4 : 1;
                if(col > widest) widest = col;
            }
            *maxw = *minw = widest * cwpx;
            n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
            return;
        }
        int line = 0, word = 0, longest = 0, in_ws = 1;
        for(u32 i = 0; i < n->tlen; i++){
            char c = n->txt[i];
            if(c == ' ' || c == '\t' || c == '\n' || c == '\r'){ if(!in_ws){ line++; in_ws = 1; } word = 0; continue; }
            if((unsigned char)c >= 0x80 && ((unsigned char)c & 0xC0) == 0x80) continue;   /* UTF-8 tail bytes */
            if(c == '&'){ const char *rep; int u = web_entity(n->txt + i, n->tlen - i, &rep); if(u > 1) i += (u32)u - 1; }
            in_ws = 0; line++; word++;
            if(word > longest) longest = word;
        }
        if(in_ws && line) line--;
        *maxw = line * cwpx;
        *minw = longest * cwpx;
        n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
        return;
    }
    const wstyle_t *st = &n->st;
    if(st->display == W_NONE){ n->cs_gen = layout_gen; n->cs_max = n->cs_min = 0; return; }
    int extra = st->p[1] + st->p[3] + st->bl + st->br + st->m[1] + st->m[3];
    if(strcmp(n->tag, "video") == 0){
        int dw, dh;
        video_size(n, view_w, &dw, &dh);
        *maxw = *minw = dw + extra;
        n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
        return;
    }
    if(strcmp(n->tag, "img") == 0){
        int dw = 0, dh = 0;
        if(!img_size(n, view_w, &dw, &dh)){
            dw = 0;
            if(n->alt && n->alt[0]) dw = (int)strlen(n->alt) * GLYPH_W(scale_of(st->size));
        }
        *maxw = *minw = dw + extra;
        if(st->max_pct && !st->width){ *minw = extra; }                   /* max-width: 100% images can shrink */
        n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
        return;
    }
    if(strcmp(n->tag, "input") == 0){
        int w = item_w(st, view_w);
        const char *ty = wnode_attr_get(n, "type");
        int chars = ty && (strcmp(ty, "checkbox") == 0 || strcmp(ty, "radio") == 0) ? 3 :
                    ty && (strcmp(ty, "submit") == 0 || strcmp(ty, "button") == 0) ? (n->value ? (int)strlen(n->value) + 2 : 8) : 20;
        if(!w) w = chars * GLYPH_W(scale_of(st->size)) + extra;
        *maxw = *minw = w;
        n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
        return;
    }
    int fixed = item_w(st, view_w);
    int run = 0, best = 0, mn = 0;
    int row = (st->display == W_FLEX && !st->flex_col && !st->grid) || (st->grid && st->grid_cols > 1);
    int k = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        int cm, cn;
        if(!c->text && (c->st.display == W_NONE || c->st.pos == 2)) continue;
        content_size(c, &cm, &cn);
        if(cn > mn) mn = cn;
        /* a <figure>'s caption wraps at the width of the picture (a table caption, in effect) */
        if(!c->text && strcmp(c->tag, "figcaption") == 0 && strcmp(n->tag, "figure") == 0) cm = cn;
        int block = !c->text && c->st.display != W_INLINE && !c->st.float_;
        if(row){ run += cm + (k++ ? st->gap : 0); continue; }
        if(block){ if(run > best) best = run; run = 0; if(cm > best) best = cm; }
        else run += cm + (c->text ? GLYPH_W(scale_of(c->st.size)) : 0);    /* a space between inline pieces */
    }
    if(run > best) best = run;
    if(st->display == W_LISTITEM) best += GLYPH_H(scale_of(st->size));
    if(st->max_w && !st->max_pct && best > st->max_w) best = st->max_w;
    if(st->min_w > best) best = st->min_w;
    *maxw = (fixed ? fixed : best + extra);
    *minw = (fixed ? (fixed < mn + extra ? fixed : mn + extra) : mn + extra);
    if(*minw > *maxw) *minw = *maxw;
    n->cs_gen = layout_gen; n->cs_max = *maxw; n->cs_min = *minw;
}

/* Lays c out as a box of exactly w at (x, y): its own width settings are already counted. */
static int box_slot_last = -1;       /* the background slot of the last block laid out */
static int box_top_last = 0;
static void layout_in_box(wnode_t *c, int x, int w, int y){
    cx = x; cw = w; cy = y;
    int sw = 0, sp = 0, sv = 0, sm = 0, smp = 0, smv = 0, sd = 0, sa = 0, sminw = 0;
    if(!c->text){
        wstyle_t *s = &c->st;
        sw = s->width; sp = s->width_pct; sv = s->width_vw;
        sm = s->max_w; smp = s->max_pct; smv = s->max_vw; sd = s->display; sa = s->auto_lr; sminw = s->min_w;
        if(strcmp(c->tag, "img") != 0){ s->width = s->width_pct = s->width_vw = 0; s->max_w = s->max_pct = s->max_vw = 0; s->min_w = 0; }
        s->auto_lr = 0;
        if(s->display == W_INLINE || s->display == W_LISTITEM) s->display = s->display == W_LISTITEM ? W_LISTITEM : W_BLOCK;
    }
    int saved_align = cur_align;
    layout_node(c);
    flush_line();
    cur_align = saved_align;
    if(!c->text){
        wstyle_t *s = &c->st;
        s->width = sw; s->width_pct = sp; s->width_vw = sv;
        s->max_w = sm; s->max_pct = smp; s->max_vw = smv; s->display = sd; s->auto_lr = sa; s->min_w = sminw;
    }
}

/* ---- flex and grid ---- */

typedef struct { wnode_t *n; int base, minw, w, grow, shrink; } flex_item_t;
#define FLEX_ITEMS 160

/* Lays out one line of flex items from y0; returns the bottom of the tallest. */
static int flex_line(flex_item_t *it, int k, int x0, int w, int gap, int justify, int y0){
    int sum = 0, grow = 0;
    for(int i = 0; i < k; i++){ sum += it[i].w; grow += it[i].grow; }
    int free = w - sum - gap * (k - 1);
    int start = x0, between = gap;
    if(free > 0 && grow > 0){
        for(int i = 0; i < k; i++) if(it[i].grow) it[i].w += free * it[i].grow / grow;
    } else if(free > 0){
        if(justify == 1) start += free / 2;
        else if(justify == 2) start += free;
        else if(justify == 3 && k > 1) between = gap + free / (k - 1);
        else if(justify == 4){ between = gap + free / k; start += free / k / 2; }
    }
    int x = start, maxy = y0;
    for(int i = 0; i < k; i++){
        layout_in_box(it[i].n, x, it[i].w, y0);
        if(cy > maxy) maxy = cy;
        x += it[i].w + between;
    }
    return maxy;
}

/* ---- grid ----
 * Column tracks from grid-template-columns (px, %, fr, auto, auto-fill), items placed by
 * grid-template-areas names, by line numbers, or in order into the next free cells. Rows are
 * as tall as their tallest item. */
#define GRID_COLS 16
#define GRID_ROWS 256
#define GRID_ITEMS 512

/* The area name's rectangle in "'a a' 'b c'": 0-based rows and columns; 0 if not there. */
static int grid_area(const char *areas, const char *name, int *r0, int *r1, int *c0, int *c1){
    if(!areas || !name) return 0;
    int nl = (int)strlen(name), row = -1, found = 0;
    *r0 = *c0 = 1 << 20; *r1 = *c1 = -1;
    for(const char *p = areas; *p; p++){
        if(*p != '"' && *p != '\'') continue;
        char q = *p++;
        row++;
        int col = 0;
        while(*p && *p != q){
            while(*p == ' ' || *p == '\t' || *p == '\n') p++;
            if(!*p || *p == q) break;
            const char *b = p;
            while(*p && *p != q && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if((int)(p - b) == nl && memcmp(b, name, (u32)nl) == 0){
                found = 1;
                if(row < *r0) *r0 = row; if(row > *r1) *r1 = row;
                if(col < *c0) *c0 = col; if(col > *c1) *c1 = col;
            }
            col++;
        }
        if(!*p) break;
    }
    return found;
}

static void flex_children(wnode_t *n, int x0, int w, int gap);
/* The items of a flex or grid container: its children, with display: contents ones replaced
 * by their own children. Out-of-flow ones are queued instead. */
static void gather_items(wnode_t *n, wnode_t **items, int *k, int max, int depth){
    for(wnode_t *c = n->first; c && *k < max; c = c->next){
        if(out_of_flow(c)){ layout_node(c); continue; }
        if(!c->text && c->st.contents && c->st.display != W_NONE && depth < 8){ gather_items(c, items, k, max, depth + 1); continue; }
        if(blank(c)) continue;
        items[(*k)++] = c;
    }
}
/* Without memory for the grid: its items stacked. */
static void flex_children_column(wnode_t *n, int x0, int w){
    int saved = n->st.flex_col;
    short g = n->st.grid;
    n->st.flex_col = 1; n->st.grid = 0;
    flex_children(n, x0, w, n->st.rgap);
    n->st.flex_col = saved; n->st.grid = g;
}

static void grid_children(wnode_t *n, int x0, int w){
    const wstyle_t *st = &n->st;
    int gap = st->gap, rgap = st->rgap;
    int cols = st->grid_cols, auto_fill = st->grid_min > 0;
    if(auto_fill){ cols = (w + gap) / (st->grid_min + gap); if(cols < 1) cols = 1; }
    if(cols < 1) cols = 1;
    if(cols > GRID_COLS) cols = GRID_COLS;
    /* placement (per call: an item can hold a grid of its own) */
    struct grid_work {
        wnode_t *items[GRID_ITEMS];
        short pr0[GRID_ITEMS], pr1[GRID_ITEMS], pc0[GRID_ITEMS], pc1[GRID_ITEMS];
        u8 occ[GRID_ROWS][GRID_COLS];
        int row_bot[GRID_ROWS + 1];
    } *gw = kmalloc(sizeof(struct grid_work));
    if(!gw){ flex_children_column(n, x0, w); return; }
    wnode_t **items = gw->items;
    short *pr0 = gw->pr0, *pr1 = gw->pr1, *pc0 = gw->pc0, *pc1 = gw->pc1;
    u8 (*occ)[GRID_COLS] = gw->occ;
    int *row_bot = gw->row_bot;
    int k = 0;
    gather_items(n, items, &k, GRID_ITEMS, 0);
    memset(gw->occ, 0, sizeof gw->occ);
    int nrows = 0;
    /* explicitly placed items first, then the rest in order */
    for(int pass = 0; pass < 2; pass++){
        int cur_r = 0, cur_c = 0;
        for(int i = 0; i < k; i++){
            wnode_t *c = items[i];
            const wstyle_t *cs = &c->st;
            int r0 = -1, r1 = -1, c0 = -1, c1 = -1;
            int explicit_ = 0;
            if(!c->text && cs->garea && grid_area(st->gareas, cs->garea, &r0, &r1, &c0, &c1)) explicit_ = 1;
            else if(!c->text && (cs->gcs || cs->grs)){
                int span = cs->gcspan > 0 ? cs->gcspan : 1;
                c0 = cs->gcs > 0 ? cs->gcs - 1 : cs->gcs < 0 ? cols + 1 + cs->gcs - 1 : -1;
                c1 = cs->gce > 0 ? cs->gce - 2 : cs->gce < 0 ? cols + cs->gce : (c0 >= 0 ? c0 + span - 1 : -1);
                r0 = cs->grs > 0 ? cs->grs - 1 : -1;
                int rs = cs->grspan > 0 ? cs->grspan : 1;
                r1 = cs->gre > 0 ? cs->gre - 2 : (r0 >= 0 ? r0 + rs - 1 : -1);
                explicit_ = r0 >= 0 || c0 >= 0;
            }
            if(explicit_ != (pass == 0)) continue;
            if(pass == 1 || r0 < 0 || c0 < 0){
                /* auto: the next free cells (row-major) wide enough for its span */
                int span = c->text ? 1 : (c->st.gcspan > 0 ? c->st.gcspan : (c->st.gcs < 0 && c->st.gce == 0 ? cols : 1));
                if(!c->text && c->st.gcs == 1 && c->st.gce == -1) span = cols;
                if(c0 >= 0 && c1 >= c0) span = c1 - c0 + 1;
                if(span > cols) span = cols;
                int rs = (!c->text && c->st.grspan > 0) ? c->st.grspan : 1;
                int placed = 0;
                for(int r = (r0 >= 0 ? r0 : cur_r); r < GRID_ROWS && !placed; r++){
                    for(int cc = (r == cur_r && c0 < 0 ? cur_c : 0); cc + span <= cols; cc++){
                        if(c0 >= 0 && cc != c0) continue;
                        int ok = 1;
                        for(int q = 0; q < span && ok; q++) for(int rr = r; rr < r + rs && rr < GRID_ROWS && ok; rr++) if(occ[rr][cc + q]) ok = 0;
                        if(ok){ r0 = r; r1 = r + rs - 1; c0 = cc; c1 = cc + span - 1; placed = 1; break; }
                    }
                    if(c0 >= 0 && !placed && r > GRID_ROWS - 2) break;
                }
                if(!placed){ r0 = r1 = nrows < GRID_ROWS ? nrows : GRID_ROWS - 1; c0 = 0; c1 = cols - 1; }
                cur_r = r0; cur_c = c1 + 1;
                if(cur_c >= cols){ cur_c = 0; cur_r++; }
            }
            if(c0 < 0) c0 = 0;
            if(c1 < c0) c1 = c0;
            if(c1 >= cols) c1 = cols - 1;
            if(r0 < 0) r0 = 0;
            if(r1 < r0) r1 = r0;
            if(r1 >= GRID_ROWS) r1 = GRID_ROWS - 1;
            if(r0 >= GRID_ROWS) r0 = GRID_ROWS - 1;
            for(int rr = r0; rr <= r1; rr++) for(int q = c0; q <= c1; q++) occ[rr][q] = 1;
            pr0[i] = (short)r0; pr1[i] = (short)r1; pc0[i] = (short)c0; pc1[i] = (short)c1;
            if(r1 + 1 > nrows) nrows = r1 + 1;
        }
    }
    /* column widths: fixed tracks, then auto tracks as wide as their items want, then the
       rest shared by fr (with no fr tracks, the auto tracks share it) */
    int colw[GRID_COLS], fixed_sum = 0, fr_sum = 0, auto_n = 0;
    int autoneed[GRID_COLS];
    for(int i = 0; i < cols; i++){
        int t = auto_fill ? -10 : st->gtr[i];
        autoneed[i] = 0;
        if(t >= GTR_PCT){ colw[i] = (int)((long long)w * (t - GTR_PCT) / 1000); fixed_sum += colw[i]; }
        else if(t > 0){ colw[i] = t; fixed_sum += t; }
        else if(t < 0){ colw[i] = 0; fr_sum += -t; }
        else { colw[i] = 0; auto_n++; }
    }
    if(auto_n){
        for(int i = 0; i < k; i++){
            if(pc0[i] != pc1[i]) continue;
            int t = auto_fill ? -10 : st->gtr[pc0[i]];
            if(t != 0) continue;
            int mx, mn; content_size(items[i], &mx, &mn);
            if(mx > autoneed[pc0[i]]) autoneed[pc0[i]] = mx;
        }
        for(int i = 0; i < cols; i++) if(!auto_fill && st->gtr[i] == 0){ colw[i] = autoneed[i]; fixed_sum += autoneed[i]; }
    }
    int free = w - fixed_sum - gap * (cols - 1);
    if(free > 0){
        if(fr_sum){ for(int i = 0; i < cols; i++){ int t = auto_fill ? -10 : st->gtr[i]; if(t < 0) colw[i] = (int)((long long)free * -t / fr_sum); } }
        else if(auto_n){ for(int i = 0; i < cols; i++) if(st->gtr[i] == 0) colw[i] += free / auto_n; }
    } else if(fixed_sum > 0){
        /* too wide: scale everything down to fit */
        int room = w - gap * (cols - 1);
        if(room < cols) room = cols;
        for(int i = 0; i < cols; i++) colw[i] = (int)((long long)colw[i] * room / fixed_sum);
    }
    int colx[GRID_COLS + 1];
    colx[0] = x0;
    for(int i = 0; i < cols; i++) colx[i + 1] = colx[i] + colw[i] + gap;

    /* rows, top to bottom */
    int y = cy, any = 0;
    for(int r = 0; r < nrows; r++) row_bot[r] = -1;
    for(int r = 0; r < nrows; r++){
        int used_row = 0;
        for(int i = 0; i < k; i++) if(pr0[i] <= r && pr1[i] >= r){ used_row = 1; break; }
        if(!used_row){ row_bot[r] = y; continue; }
        if(any) y += rgap;
        any = 1;
        int bot = y;
        for(int i = 0; i < k; i++){
            if(pr0[i] != r) continue;
            int x = colx[pc0[i]];
            int iw = colx[pc1[i] + 1] - gap - x;
            if(iw < 8) iw = 8;
            wnode_t *c = items[i];
            if(c->text){ cx = x; cw = iw; cy = y; layout_node(c); flush_line(); }
            else layout_in_box(c, x, iw, y);
            if(pr1[i] == r && cy > bot) bot = cy;
            if(pr1[i] > r){ if(cy > row_bot[pr1[i]]) row_bot[pr1[i]] = cy; }
        }
        if(row_bot[r] > bot) bot = row_bot[r];
        row_bot[r] = bot;
        y = bot;
    }
    kfree(gw);
    cy = y;
    cx = x0; cw = w;
}

/* A flex row: items take the width their content wants (or their CSS width/basis), grow into
 * free space by flex-grow, are placed by justify-content, and wrap onto more lines when
 * they do not fit (instead of being squeezed into unreadable columns). */
static void flex_children(wnode_t *n, int x0, int w, int gap){
    const wstyle_t *st = &n->st;
    if(st->grid && (st->grid_cols > 1 || st->grid_min > 0 || st->gareas)){ grid_children(n, x0, w); return; }
    if(st->flex_col || st->grid){
        /* a column: the items stack like blocks, gap apart */
        int first = 1;
        int g = st->grid ? st->rgap : gap;
        for(wnode_t *c = n->first; c; c = c->next){
            if(out_of_flow(c)){ layout_node(c); continue; }
            if(blank(c)) continue;
            if(!first) cy += g;
            first = 0;
            if(c->text){ cx = x0; cw = w; layout_node(c); flush_line(); continue; }
            int fixed = item_w(&c->st, w);
            int iw = fixed ? fixed : w;
            int ix = x0;
            if(fixed && fixed < w){
                /* align-items is not known: centring follows text-align or auto margins */
                if(c->st.auto_lr == 3 || st->justify == 1) ix += (w - fixed) / 2;
            }
            if(fixed) layout_in_box(c, ix, iw, cy);
            else {
                int sd = c->st.display;
                if(sd == W_INLINE) c->st.display = W_BLOCK;
                cx = x0; cw = w;
                layout_node(c);
                flush_line();
                c->st.display = sd;
            }
        }
        cx = x0; cw = w;
        return;
    }
    flex_item_t mine[FLEX_ITEMS];
    wnode_t *fl[FLEX_ITEMS]; int nfl = 0;
    gather_items(n, fl, &nfl, FLEX_ITEMS, 0);
    int k = 0;
    for(int fi = 0; fi < nfl; fi++){
        wnode_t *c = fl[fi];
        int mx, mn;
        content_size(c, &mx, &mn);
        int fixed = 0;
        if(!c->text){
            const wstyle_t *cs = &c->st;
            if(cs->basis_set){
                int b = cs->basis + (int)((long long)w * cs->basis_pct / 1000);
                if(!cs->border_box) b += cs->p[1] + cs->p[3] + cs->bl + cs->br;
                fixed = b + cs->m[1] + cs->m[3];
                if(fixed <= 0) fixed = 1;               /* a 0 basis: it grows into the free space */
            } else fixed = item_w(cs, w);
        }
        int base = fixed ? fixed : mx + 2;
        if(base > w) base = w;
        if(mn > w) mn = w;
        if(!c->text && c->st.basis_set && mn > base) mn = base;       /* min-width auto aside: let it fit the line */
        if(!c->text && c->st.no_shrink && mn < base) mn = base;
        int grow = c->text ? 0 : c->st.flex_grow;
        if(st->trow && !c->text && c->st.tcell && !fixed) grow = 1;
        mine[k] = (flex_item_t){ c, base, mn, base, grow, 1 };
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
                int cut = room > 0 ? (int)((long long)over * (mine[q].base - mine[q].minw) / room) : 0;
                mine[q].w = mine[q].base - cut;
                if(mine[q].w < mine[q].minw) mine[q].w = mine[q].minw;
            }
        } else for(int q = i; q < j; q++) mine[q].w = mine[q].base;
        y = flex_line(&mine[i], j - i, x0, w, gap, st->justify, y);
        if(j < k) y += st->rgap ? st->rgap : 0;
        i = j;
    }
    cy = y;
    cx = x0; cw = w;
}

/* ---- floats and inline-blocks: boxes side by side ---- */

static int has_block_child(const wnode_t *n){
    for(const wnode_t *c = n->first; c; c = c->next){
        if(c->text || c->st.display == W_NONE) continue;
        if(c->st.display == W_BLOCK || c->st.display == W_FLEX || c->st.display == W_LISTITEM || c->st.float_) return 1;
        if(c->st.display == W_INLINE && !c->st.inline_block && has_block_child(c)) return 1;
    }
    return 0;
}
static int rowish(const wnode_t *c){
    if(c->text) return 0;
    const wstyle_t *s = &c->st;
    if(s->display == W_NONE || s->pos == 2) return 0;
    if(s->float_) return 1;
    if(s->inline_block){
        if(s->display == W_FLEX) return 1;
        if(s->width || s->width_pct || s->width_vw) return 1;
        if(strcmp(c->tag, "img") != 0 && has_block_child(c)) return 1;
    }
    return 0;
}

#define ROW_ITEMS 96
/* Lays out a run of floats / inline-blocks starting at *pc, and (beside floats) the content
 * that follows them. Advances *pc past what it laid out. */
static void row_group(wnode_t **pc){
    wnode_t *items[ROW_ITEMS]; int k = 0;
    wnode_t *c = *pc;
    int any_float = 0;
    while(c && k < ROW_ITEMS && (rowish(c) || blank(c))){
        if(out_of_flow(c)) layout_node(c);
        else if(rowish(c)){ items[k++] = c; if(c->st.float_) any_float = 1; }
        c = c->next;
    }
    flush_line();
    int x0 = cx, W = cw, top = cy;
    int lx = x0, rx = x0 + W, line_top = top, line_bottom = top;
    int align = cur_align;
    /* first work out where each box goes, line by line, then lay them out */
    int line_start = 0;
    int pos[ROW_ITEMS], wid[ROW_ITEMS], line_of[ROW_ITEMS], nlines = 0;
    for(int i = 0; i < k; i++){
        wnode_t *it = items[i];
        int mx, mn;
        content_size(it, &mx, &mn);
        int fixed = item_w(&it->st, W);
        int w = fixed ? fixed : mx + 2;
        if(w > W) w = W;
        if(w < 8) w = 8;
        int right = it->st.float_ == 2;
        int cleared = it->st.float_ && (((it->st.clear & 1) && lx > x0) || ((it->st.clear & 2) && rx < x0 + W));
        if(cleared || ((right ? rx - w < lx : lx + w > rx) && (lx > x0 || rx < x0 + W))){
            nlines++; lx = x0; rx = x0 + W; line_start = i;
        }
        (void)line_start;
        if(right){ pos[i] = rx - w; rx -= w; } else { pos[i] = lx; lx += w; }
        wid[i] = w; line_of[i] = nlines;
    }
    nlines++;
    /* text-align moves lines of inline-blocks (not floats) */
    for(int L = 0; L < nlines && align; L++){
        int minx = x0 + W, maxx = x0, fl = 0;
        for(int i = 0; i < k; i++) if(line_of[i] == L){ if(items[i]->st.float_) fl = 1; if(pos[i] < minx) minx = pos[i]; if(pos[i] + wid[i] > maxx) maxx = pos[i] + wid[i]; }
        if(fl || maxx <= minx) continue;
        int free = W - (maxx - x0);
        int sh = align == 1 ? free / 2 : free;
        if(sh > 0) for(int i = 0; i < k; i++) if(line_of[i] == L) pos[i] += sh;
    }
    int cur_line = 0;
    for(int i = 0; i < k; i++){
        if(line_of[i] != cur_line){ cur_line = line_of[i]; line_top = line_bottom; }
        layout_in_box(items[i], pos[i], wid[i], line_top);
        if(cy > line_bottom) line_bottom = cy;
    }
    /* what is left beside the floats of the last line: the next block, or the inline run */
    int left_used = 0, right_used = 0;
    int all_floats = 1;
    for(int i = 0; i < k; i++) if(!items[i]->st.float_) all_floats = 0;
    if(all_floats && nlines > 1) line_top = top;          /* stacked floats: the text runs beside all of them */
    for(int i = 0; i < k; i++) if((line_of[i] == nlines - 1 || all_floats) && items[i]->st.float_){
        if(items[i]->st.float_ == 1 && pos[i] + wid[i] - x0 > left_used) left_used = pos[i] + wid[i] - x0;
        if(items[i]->st.float_ == 2 && x0 + W - pos[i] > right_used) right_used = x0 + W - pos[i];
    }
    int room = W - left_used - right_used;
    if(any_float && c && (left_used || right_used) && room >= W / 4 && room >= CSSPX(120)){
        /* what follows sits beside the floats: each block, or run of inline content, that starts
           above their bottom is laid out in the room left (a block that starts below goes back
           to the full width). clear, or another float, ends it. */
        int fx = x0 + left_used;
        int float_bottom = line_bottom;
        cy = line_top;
        while(c && cy < float_bottom){
            if(!c->text && c->st.display != W_NONE && (rowish(c) || c->st.clear)) break;
            cx = fx; cw = room;
            nfrag = 0; used = 0; pending_space = 0;
            if(!c->text && (c->st.display == W_BLOCK || c->st.display == W_FLEX || c->st.display == W_LISTITEM)){
                /* the block beside the float: a margin that only made room for the float goes */
                wstyle_t *s = &c->st;
                int ml = s->m[3], mr = s->m[1];
                if(left_used && ml >= left_used / 2) s->m[3] = 0;
                if(right_used && mr >= right_used / 2) s->m[1] = 0;
                int sw = s->width, sp = s->width_pct;
                if(sp >= 500 || s->width > room){ s->width = 0; s->width_pct = 0; }
                layout_node(c);
                flush_line();
                s->m[3] = ml; s->m[1] = mr; s->width = sw; s->width_pct = sp;
                c = c->next;
            } else {
                /* inline content: up to the next block */
                while(c && (c->text || (c->st.display == W_INLINE && !rowish(c)) || c->st.display == W_NONE)){
                    layout_node(c);
                    c = c->next;
                }
                flush_line();
            }
        }
        if(cy > line_bottom) line_bottom = cy;
    }
    cx = x0; cw = W; cy = line_bottom;
    nfrag = 0; used = 0; pending_space = 0;
    *pc = c;
}

static int has_cells(const wnode_t *n){
    int cells = 0;
    for(const wnode_t *c = n->first; c; c = c->next) if(!c->text && c->st.display != W_NONE && c->st.tcell) cells++;
    return cells >= 2;
}

/* The children of a block, with runs of floats and inline-blocks side by side. */
static void layout_children(wnode_t *n){
    wnode_t *c = n->first;
    while(c){
        if(rowish(c)){ row_group(&c); continue; }
        layout_node(c);
        c = c->next;
    }
}

/* ---- tables ---- */

#define TABLE_COLS   32
#define TABLE_DEPTH  8
#define TABLE_ROWS   2048
typedef struct { int x, w; int col_w[TABLE_COLS]; int ncols; } table_plan_t;
static table_plan_t table_stack[TABLE_DEPTH];
static int table_depth = 0;
static wnode_t *row_buf[TABLE_DEPTH][TABLE_ROWS];

static int cell_span(wnode_t *c){
    const char *v = wnode_attr_get(c, "colspan");
    if(!v) return 1;
    int s = 0;
    while(*v >= '0' && *v <= '9'){ s = s * 10 + (*v - '0'); v++; }
    return s < 1 ? 1 : (s > TABLE_COLS ? TABLE_COLS : s);
}
static int is_cell(const wnode_t *c){ return !c->text && (strcmp(c->tag, "td") == 0 || strcmp(c->tag, "th") == 0); }

/* The rows of a table: direct <tr> children and rows inside <thead>/<tbody>/<tfoot>. */
static int table_collect_rows(wnode_t *n, wnode_t **rows){
    int nr = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        if(c->text || c->st.display == W_NONE) continue;
        if(strcmp(c->tag, "tr") == 0){ if(nr < TABLE_ROWS) rows[nr++] = c; continue; }
        if(strcmp(c->tag, "thead") == 0 || strcmp(c->tag, "tbody") == 0 || strcmp(c->tag, "tfoot") == 0){
            for(wnode_t *r = c->first; r; r = r->next)
                if(!r->text && strcmp(r->tag, "tr") == 0 && r->st.display != W_NONE && nr < TABLE_ROWS) rows[nr++] = r;
        }
    }
    return nr;
}

/* The columns' wanted widths (max) and narrowest (min) widths; returns the column count. */
static int table_needs(wnode_t *n, int w, int *need, int *least){
    for(int i = 0; i < TABLE_COLS; i++){ need[i] = 0; least[i] = 0; }
    int ncols = 0;
    wnode_t **rows = row_buf[table_depth < TABLE_DEPTH ? table_depth : TABLE_DEPTH - 1];
    int nrows = table_collect_rows(n, rows);
    for(int pass = 0; pass < 2; pass++)          /* single cells first, then the spanning ones */
    for(int ri = 0; ri < nrows; ri++){
        int col = 0;
        for(wnode_t *c = rows[ri]->first; c; c = c->next){
            if(!is_cell(c)) continue;
            int span = cell_span(c);
            if(c->st.display == W_NONE){ col += span; continue; }
            if((span > 1) != pass){ col += span; continue; }
            int mx, mn;
            content_size(c, &mx, &mn);
            if(c->st.width_pct && !c->st.width){ int p = (int)((long long)w * c->st.width_pct / 1000); if(p > mx || span == 1) mx = p; }
            if(mx < 8) mx = 8;
            if(span == 1){
                if(col < TABLE_COLS){ if(mx > need[col]) need[col] = mx; if(mn > least[col]) least[col] = mn; }
            } else {
                int have = 0, haven = 0;
                for(int k = 0; k < span && col + k < TABLE_COLS; k++){ have += need[col + k]; haven += least[col + k]; }
                if(mx > have) for(int k = 0; k < span && col + k < TABLE_COLS; k++) need[col + k] += (mx - have) / span;
                if(mn > haven) for(int k = 0; k < span && col + k < TABLE_COLS; k++) least[col + k] += (mn - haven) / span;
            }
            col += span;
        }
        if(col > ncols) ncols = col > TABLE_COLS ? TABLE_COLS : col;
    }
    return ncols;
}

/* Works out the column widths and remembers them. auto_w: the table has no width of its own,
 * so it is only as wide as its columns want. */
static void table_plan(wnode_t *n, int x0, int w, int auto_w){
    if(table_depth >= TABLE_DEPTH) return;
    int need[TABLE_COLS], least[TABLE_COLS];
    int ncols = table_needs(n, w, need, least);
    if(ncols == 0){ ncols = 1; need[0] = w; least[0] = 0; }
    int sum = 0, minsum = 0;
    for(int i = 0; i < ncols; i++){ sum += need[i]; minsum += least[i]; }
    table_plan_t *p = &table_stack[table_depth++];
    p->x = x0; p->w = w; p->ncols = ncols;
    if(sum <= w){
        /* extra width (a table with a width) goes to the columns in proportion */
        int extra = auto_w ? 0 : w - sum, given = 0;
        for(int i = 0; i < ncols; i++){
            int add = sum ? (int)((long long)extra * need[i] / sum) : extra / ncols;
            p->col_w[i] = need[i] + add; given += p->col_w[i];
        }
        if(!auto_w && given < w) p->col_w[ncols - 1] += w - given;
    } else if(minsum <= w){
        /* everything at its narrowest, then the rest shared by how much more each wants */
        int rest = w - minsum, want = sum - minsum, given = 0;
        for(int i = 0; i < ncols; i++){
            p->col_w[i] = least[i] + (want ? (int)((long long)rest * (need[i] - least[i]) / want) : 0);
            given += p->col_w[i];
        }
        if(given < w) p->col_w[ncols - 1] += w - given;
    } else {
        int usedw = 0;
        for(int i = 0; i < ncols; i++){ p->col_w[i] = (int)((long long)least[i] * w / (minsum ? minsum : 1)); if(p->col_w[i] < 16) p->col_w[i] = 16; usedw += p->col_w[i]; }
        for(int i = ncols - 1; usedw > w && i >= 0; i--){
            int cut = usedw - w;
            if(cut > p->col_w[i] - 16) cut = p->col_w[i] - 16;
            if(cut > 0){ p->col_w[i] -= cut; usedw -= cut; }
        }
    }
}

/* A table row: lays its cells side by side, and stretches their backgrounds to the row. */
static void layout_tr(wnode_t *n){
    wstyle_t *st = &n->st;
    flush_line();
    cy += st->m[0];
    table_plan_t *p = table_depth > 0 ? &table_stack[table_depth - 1] : NULL;
    int row_y = cy;
    int col = 0, x = p ? p->x : cx;
    int max_cy = cy;
    int saved_align = cur_align;
    int row_slot = st->bg_set ? bg_slot() : -1;
    int slots[TABLE_COLS], tops[TABLE_COLS], nslots = 0;
    for(wnode_t *c = n->first; c; c = c->next){
        if(!is_cell(c)) continue;
        int span = cell_span(c);
        if(c->st.display == W_NONE){ if(p) col += span; continue; }
        int wsum = 0;
        if(p){ for(int k = 0; k < span && col + k < p->ncols; k++) wsum += p->col_w[col + k]; }
        else wsum = cw;
        int saved_cx = cx, saved_cw = cw;
        cx = x;
        cw = wsum > 0 ? wsum : 40;
        cy = row_y;
        cur_align = st->align;
        box_slot_last = -1;
        int sw = c->st.width, sp = c->st.width_pct;
        c->st.width = 0; c->st.width_pct = 0;        /* the plan already gave the cell its width */
        layout_node(c);                 /* the cell lays itself as a block: padding, border, bg */
        c->st.width = sw; c->st.width_pct = sp;
        if(box_slot_last >= 0 && nslots < TABLE_COLS){ slots[nslots] = box_slot_last; tops[nslots++] = box_top_last; }
        if(cy > max_cy) max_cy = cy;
        cx = saved_cx; cw = saved_cw;
        x += wsum;
        if(p) col += span;
    }
    if(st->height > 0 && row_y + st->height > max_cy) max_cy = row_y + st->height;
    /* cells are as tall as their row */
    for(int i = 0; i < nslots; i++) if(slots[i] < ncmds && cmds[slots[i]].h > 0) cmds[slots[i]].h = max_cy - tops[i];
    if(row_slot >= 0) bg_fill(row_slot, p ? p->x : cx, row_y, x - (p ? p->x : cx), max_cy - row_y, st->bg);
    cy = max_cy + st->m[2];
    cur_align = saved_align;
}

/* ---- form controls ---- */

static void layout_input(wnode_t *n, wstyle_t *st){
    const char *type = wnode_attr_get(n, "type");
    int sc = scale_of(st->size);
    int fixed = item_w(st, cw);
    int button = type && (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 || strcmp(type, "reset") == 0);
    if(type && strcmp(type, "image") == 0) return;
    if(button){
        /* a button: its label on a grey box */
        const char *label = n->value && n->value[0] ? n->value : (strcmp(type, "reset") == 0 ? "Reset" : "Submit");
        char pad[96]; u32 k = 0;
        pad[k++] = ' ';
        for(const char *q = label; *q && k < 90; q++) pad[k++] = *q;
        pad[k++] = ' '; pad[k] = 0;
        wstyle_t box = *st;
        box.color = st->bg_set ? st->color : 0x000000;     /* a plain button: dark text on its grey face */
        box.underline = 0;
        int saved_set = cur_bg_set; u32 saved_bg = cur_bg;
        cur_bg_set = 1; cur_bg = st->bg_set ? st->bg : 0xE9E9ED;
        cur_dom = n;
        add_piece(pad, (int)k, &box, cur_link);
        cur_bg_set = saved_set; cur_bg = saved_bg;
        pending_space = 1;
        return;
    }
    if(type && (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0)){
        wstyle_t box = *st;
        cur_dom = n;
        if(strcmp(type, "radio") == 0) add_piece(wnode_attr_get(n, "checked") ? "(*)" : "( )", 3, &box, cur_link);
        else add_piece(wnode_attr_get(n, "checked") ? "[x]" : "[ ]", 3, &box, cur_link);
        pending_space = 1;
        return;
    }
    /* a text box: its value, or the placeholder in grey, on a light box */
    int chars = 20;
    if(fixed) chars = (fixed - st->p[1] - st->p[3]) / GLYPH_W(sc);
    const char *sz = wnode_attr_get(n, "size");
    if(!fixed && sz){ int v = 0; for(const char *q = sz; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0'); if(v > 0) chars = v; }
    if(chars < 3) chars = 3;
    if(chars > 90) chars = 90;
    char pad[96];
    int k = 0;
    const char *shown = n->value && n->value[0] ? n->value : (n->alt ? n->alt : "");
    int has_value = n->value && n->value[0];
    int pw = type && strcmp(type, "password") == 0 && has_value;
    pad[k++] = ' ';
    for(const char *q = shown; *q && k < chars - 1; q++) pad[k++] = pw ? '*' : *q;
    while(k < chars) pad[k++] = ' ';
    pad[k] = 0;
    wstyle_t box = *st;
    /* a plain text box is light with dark text, whatever colour the page's text is */
    box.color = has_value ? (st->bg_set ? st->color : 0x202124) : 0x757575;
    box.underline = 0;
    int saved_set = cur_bg_set; u32 saved_bg = cur_bg;
    cur_bg_set = 1;
    cur_bg = st->bg_set ? st->bg : 0xF1F3F4;
    if(st->bg_set && st->bg == st->under) cur_bg = (st->bg & 0xFEFEFE) > 0x808080 ? 0xEDEEF0 : 0x3A3F45;   /* keep the box visible */
    cur_dom = n;
    add_piece(pad, k, &box, cur_link);
    cur_bg_set = saved_set; cur_bg = saved_bg;
}

/* ---- the tree ---- */

/* Halfway between the text colour and what is behind it: for alt text. */
static u32 blend_gray(const wstyle_t *st){
    u32 a = st->color, b = st->bg_set ? st->bg : st->under;
    return ((a & 0xFEFEFE) >> 1) + ((b & 0xFEFEFE) >> 1);
}

/* Lays out the boxes queued since mark against the containing block (x, y, w, h). */
static void process_abs(int mark, int bx, int by, int bw, int bh){
    for(int i = mark; i < nabs; i++){
        absq_t e = absq[i];
        wnode_t *n = e.n;
        wstyle_t *st = &n->st;
        int o[4];
        for(int s = 0; s < 4; s++) o[s] = st->off[s] + (int)((long long)(s & 1 ? bw : bh) * st->off_pct[s] / 1000);
        int has_t = st->off_set & 1, has_r = st->off_set & 2, has_b = st->off_set & 4, has_l = st->off_set & 8;
        int w = item_w(st, bw);
        if(!w && has_l && has_r) w = bw - o[1] - o[3];
        if(!w){ int mx, mn; content_size(n, &mx, &mn); w = mx + 2; }
        if(w > bw && bw > 0) w = bw;
        if(w < 8) w = 8;
        int x = has_l ? bx + o[3] : has_r ? bx + bw - o[1] - w : e.sx;
        int y = has_t ? by + o[0] : e.sy;
        int scx = cx, scw = cw, scy = cy, sused = used, spend = pending_space, salign = cur_align, slink = cur_link, sbg = cur_bg_set;
        u32 sbgc = cur_bg;
        int snf = nfrag;
        nfrag = 0; used = 0; pending_space = 0; cur_link = e.link; cur_bg_set = 0;
        int c0 = ncmds;
        wnode_t *saved_now = abs_now;
        abs_now = n;
        top_depth++;
        layout_in_box(n, x, w, y);
        top_depth--;
        abs_now = saved_now;
        if(!has_t && has_b){
            int h = cy - y, dy = by + bh - o[2] - h - y;
            for(int k = c0; k < ncmds; k++) cmds[k].y += dy;
        }
        cx = scx; cw = scw; cy = scy; used = sused; pending_space = spend; cur_align = salign; cur_link = slink;
        cur_bg_set = sbg; cur_bg = sbgc; nfrag = snf;
    }
    nabs = mark;
}

static void layout_node(wnode_t *n){
    wstyle_t *st = &n->st;
    if(n->text){
        cur_dom = n->parent;
        add_text(n->txt, n->tlen, st, cur_link);
        return;
    }
    cur_dom = n;
    if(st->display == W_NONE) return;
    if(st->pos == 2 && n != abs_now && nabs < ABS_MAX){
        absq[nabs++] = (absq_t){ n, cx + (nfrag ? used : 0), cy, cur_link };
        return;
    }
    if(strcmp(n->tag, "tr") == 0){ layout_tr(n); return; }
    if(strcmp(n->tag, "br") == 0){
        if(nfrag == 0){ cy += GLYPH_H(scale_of(st->size)) + GLYPH_H(scale_of(st->size)) / 5; return; }   /* an empty line */
        flush_line();
        return;
    }
    if(strcmp(n->tag, "input") == 0){ layout_input(n, st); return; }
    if(strcmp(n->tag, "video") == 0 || strcmp(n->tag, "audio") == 0){
        /* a block of its own: black, and the browser draws the picture over it */
        if(strcmp(n->tag, "audio") == 0) return;            /* no controls drawn: nothing to show */
        int dw, dh;
        video_size(n, cw - st->m[1] - st->m[3], &dw, &dh);
        flush_line();
        cy += st->m[0];
        int ox = 0, room = cw - st->m[1] - st->m[3] - dw;
        if(st->auto_lr == 3 || cur_align == 1) ox = room / 2;
        if(ox < 0) ox = 0;
        web_cmd_t vc = { .kind = WEB_CMD_VIDEO, .x = cx + st->m[3] + ox, .y = cy, .w = dw, .h = dh, .color = 0x000000,
                         .link = -1, .dom = n, .img = -1 };
        emit(&vc);
        cy += dh + st->m[2];
        return;
    }
    if(strcmp(n->tag, "img") == 0){
        int idx = n->img ? n->img - 1 : -1;
        img_mark_wanted(idx);
        int dw = 0, dh = 0;
        int known = img_size(n, cw - st->m[1] - st->m[3], &dw, &dh);
        int state = idx >= 0 ? web_image_state(idx) : 2;
        if(known){
            int drawn = state == 1 ? idx : -1;          /* not here yet: an empty box keeps its place */
            if(st->display == W_BLOCK || st->display == W_FLEX || st->display == W_LISTITEM){
                flush_line();
                cy += st->m[0];
                int ox = 0, room = cw - st->m[1] - st->m[3] - dw;
                if(st->auto_lr == 3 || cur_align == 1) ox = room / 2;
                else if(st->auto_lr == 2 || cur_align == 2) ox = room;
                if(ox < 0) ox = 0;
                if(drawn >= 0){
                    web_cmd_t im = { .kind = WEB_CMD_IMAGE, .x = cx + st->m[3] + ox, .y = cy, .w = dw, .h = dh,
                                     .link = cur_link, .dom = n, .img = drawn };
                    emit(&im);
                }
                cy += dh + st->m[2];
                return;
            }
            add_box(dw, dh, drawn, cur_link, n, st->m[3], st->m[1]);
            return;
        }
        if(state == 0) return;                          /* loading, size unknown: nothing yet */
        /* could not be shown: the alt text, as browsers do */
        if(n->alt && n->alt[0]){
            wstyle_t muted = *st;
            muted.color = blend_gray(st);
            add_text(n->alt, (u32)strlen(n->alt), &muted, cur_link);
            pending_space = 1;
        }
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
        if(st->m[3] > 0 && nfrag > 0) used += st->m[3];
        for(wnode_t *c = n->first; c; c = c->next){
            if(rowish(c)){ row_group(&c); if(!c) break; layout_node(c); continue; }
            layout_node(c);
        }
        if(st->m[1] > 0 && nfrag > 0) used += st->m[1];
        if(strcmp(n->tag, "button") == 0 || strcmp(n->tag, "select") == 0 || strcmp(n->tag, "textarea") == 0) pending_space = 1;
        cur_bg_set = saved_bg_set;
        cur_bg = saved_bg;
        cur_link = saved_link;
        return;
    }

    /* block (or list item, flex, grid) */
    flush_line();
    cy += st->m[0];
    int saved_cx = cx, saved_cw = cw, saved_align = cur_align;
    int saved_bg_set = cur_bg_set;
    cur_bg_set = 0;
    int box_top = cy;
    int bt = st->bt, br = st->br, bb = st->bb, bl = st->bl;
    int bw = bl + br;
    int is_table = strcmp(n->tag, "table") == 0 && table_depth < TABLE_DEPTH;
    /* the content width: width (px, % of the container, vw) capped by max-width */
    int avail = cw - st->m[1] - st->m[3];
    int extra = st->p[1] + st->p[3] + bw;
    int content = avail - extra;
    int want = st->width + (int)((long long)cw * st->width_pct / 1000) + view_w * st->width_vw / 1000;
    if(want > 0){ content = st->border_box ? want - extra : want; }
    int maxw = st->max_w + (int)((long long)cw * st->max_pct / 1000) + view_w * st->max_vw / 1000;
    if(maxw > 0){ int m = st->border_box ? maxw - extra : maxw; if(content > m) content = m; }
    if(st->min_w > 0 && content < st->min_w - (st->border_box ? extra : 0)) content = st->min_w - (st->border_box ? extra : 0);
    if(content > avail - extra) content = avail - extra;          /* there is no sideways scrolling */
    if(is_table && want <= 0){
        /* a table without a width is as wide as its columns want */
        int mx, mn;
        content_size(n, &mx, &mn);
        int inner = mx - st->m[1] - st->m[3] - extra;
        int need[TABLE_COLS], least[TABLE_COLS];
        int nc = table_needs(n, content, need, least), sum = 0;
        for(int i = 0; i < nc; i++) sum += need[i];
        if(sum > inner) inner = sum;
        if(inner > 0 && inner < content) content = inner;
    }
    if(content < CSSPX(8)) content = CSSPX(8);
    int box_w = content + extra;
    int box_x = cx + st->m[3];
    if(box_w < avail){
        if(st->auto_lr == 3) box_x += (avail - box_w) / 2;            /* margin: 0 auto */
        else if(st->auto_lr == 1 && st->float_ != 1) { }
        else if(st->auto_lr == 2) box_x += avail - box_w;
    }
    int bgi = st->bg_idx ? st->bg_idx - 1 : -1;
    int bg_ready = 0;
    if(bgi >= 0){ img_mark_wanted(bgi); bg_ready = web_image_state(bgi) == 1; }
    int slot = st->bg_set && !(st->bg_stand && bg_ready) ? bg_slot() : -1;
    int img_slot = bg_ready ? bgimg_slot() : -1;           /* over the colour, under the content */
    box_slot_last = slot; box_top_last = box_top;
    int my_slot = slot;
    int abs_mark = nabs;
    cy += st->p[0] + bt;
    cx = box_x + st->p[3] + bl;
    cw = content;
    cur_align = st->align;
    if(st->display == W_LISTITEM && !st->no_bullet){
        wstyle_t bullet = *st;
        bullet.underline = 0; bullet.strike = 0;
        if(n->parent && strcmp(n->parent->tag, "ol") == 0){
            /* ordered list: a number, not a disc */
            int num = 1;
            const char *startv = wnode_attr_get(n->parent, "start");
            if(startv){ num = 0; for(const char *q = startv; *q >= '0' && *q <= '9'; q++) num = num * 10 + (*q - '0'); }
            for(wnode_t *sib = n->parent->first; sib && sib != n; sib = sib->next)
                if(!sib->text && strcmp(sib->tag, "li") == 0) num++;
            const char *vv = wnode_attr_get(n, "value");
            if(vv){ num = 0; for(const char *q = vv; *q >= '0' && *q <= '9'; q++) num = num * 10 + (*q - '0'); }
            char buf[16]; int k = 0; char tmp[12]; int t = 0;
            if(num <= 0) tmp[t++] = '0';
            for(int v = num; v > 0; v /= 10) tmp[t++] = (char)('0' + v % 10);
            while(t > 0) buf[k++] = tmp[--t];
            buf[k++] = '.';
            add_text(buf, (u32)k, &bullet, -1);
        } else {
            add_text("\x07", 1, &bullet, -1);     /* a centred filled disc */
        }
        pending_space = 1;
    }
    if(is_table) table_plan(n, cx, cw, want <= 0);
    if(st->display == W_FLEX){
        flex_children(n, cx, cw, st->gap);
    } else if(has_cells(n)){
        /* display: table-cell children sit side by side like a row */
        wstyle_t saved = *st;
        st->trow = 1; st->flex_wrap = 0; st->flex_col = 0; st->grid = 0;
        flex_children(n, cx, cw, 0);
        st->trow = saved.trow; st->flex_wrap = saved.flex_wrap; st->flex_col = saved.flex_col; st->grid = saved.grid;
    } else {
        layout_children(n);
    }
    if(is_table) table_depth--;
    flush_line();
    if(st->height > 0){
        int content_h = cy - (box_top + st->p[0] + bt);
        int target = st->border_box ? st->height - st->p[0] - st->p[2] - bt - bb : st->height;
        if(target > content_h && target < CSSPX(4000)) cy += target - content_h;
    }
    cy += st->p[2] + bb;
    int box_h = cy - box_top;
    if(my_slot >= 0){ bg_fill(my_slot, box_x, box_top, box_w, box_h, st->bg); cmds[my_slot].dom = n; cmds[my_slot].link = cur_link; }
    if(img_slot >= 0) bgimg_fill(img_slot, st, bgi, box_x + bl, box_top + bt, box_w - bl - br, box_h - bt - bb);
    if(st->pos && nabs > abs_mark){
        int save_cy = cy;
        process_abs(abs_mark, box_x + bl, box_top + bt, box_w - bl - br, box_h - bt - bb);
        cy = save_cy;
    }
    if(bt > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top, .w = box_w, .h = bt, .color = st->border_color, .link = -1, .img = -1 };
        emit(&e);
    }
    if(bb > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top + box_h - bb, .w = box_w, .h = bb, .color = st->border_color, .link = -1, .img = -1 };
        emit(&e);
    }
    if(bl > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x, .y = box_top, .w = bl, .h = box_h, .color = st->border_color, .link = -1, .img = -1 };
        emit(&e);
    }
    if(br > 0){
        web_cmd_t e = { .kind = WEB_CMD_RECT, .x = box_x + box_w - br, .y = box_top, .w = br, .h = box_h, .color = st->border_color, .link = -1, .img = -1 };
        emit(&e);
    }
    cy += st->m[2];
    cx = saved_cx; cw = saved_cw; cur_align = saved_align;
    cur_bg_set = saved_bg_set;
    cur_link = saved_link;
    box_slot_last = my_slot; box_top_last = box_top;
}

/* Computes every element's style, top-down. A hidden element's subtree is never laid out,
 * so it is not styled either. */
static wnode_t **spliced;              /* elements whose ::before/::after are in the child list */
static int nspliced, spliced_cap;

static void pe_splice(wnode_t *n){
    if(nspliced == spliced_cap){
        int nc = spliced_cap ? spliced_cap * 2 : 256;
        wnode_t **g = kmalloc(sizeof(wnode_t *) * (u32)nc);
        if(!g) return;
        if(spliced){ memcpy(g, spliced, sizeof(wnode_t *) * (u32)nspliced); kfree(spliced); }
        spliced = g; spliced_cap = nc;
    }
    spliced[nspliced++] = n;
    wnode_t *a = n->pe[0], *b = n->pe[1];
    if(a && a->st.display != W_NONE){ a->next = n->first; n->first = a; if(!n->last) n->last = a; }
    if(b && b->st.display != W_NONE){ b->next = NULL; if(n->last) n->last->next = b; else n->first = b; n->last = b; }
}

/* After the layout: the generated boxes leave the tree again (scripts see the real children). */
static void pe_unsplice(void){
    for(int i = nspliced - 1; i >= 0; i--){
        wnode_t *n = spliced[i], *a = n->pe[0], *b = n->pe[1];
        if(a && n->first == a){ n->first = a->next; if(n->last == a) n->last = NULL; a->next = NULL; }
        if(b && n->last == b){
            wnode_t *prev = NULL;
            for(wnode_t *c = n->first; c && c != b; c = c->next) prev = c;
            if(prev) prev->next = NULL; else n->first = NULL;
            n->last = prev;
        }
    }
    nspliced = 0;
}

static void style_tree(wnode_t *n, wnode_t *parent){
    int mark = css_var_mark();
    css_apply(n, parent);
    if(!n->text && n->st.display != W_NONE){
        int pe = n->pseudo ? 0 : css_pseudo(n);
        css_number_children(n);
        for(wnode_t *c = n->first; c; c = c->next) style_tree(c, n);
        if(pe) pe_splice(n);
    }
    css_var_pop(mark);
}

int web_layout(int width){
    if(!web_root) return 0;
    if(!cmds && !grow_cmds()) return 0;

    for(int i = 0; i < web_nlinks; i++){ if(web_link_list[i]) kfree(web_link_list[i]); web_link_list[i] = NULL; }
    web_nlinks = 0;

    css_prepare(width);
    style_tree(web_root, NULL);

    layout_gen++;
    ncmds = 0;
    acur = 0; aused = 0;
    nfrag = 0;
    used = 0;
    pending_space = 0;
    cur_align = 0;
    cur_link = -1;
    cur_bg_set = 0;
    table_depth = 0;
    nabs = 0; abs_now = NULL; top_depth = 0;
    view_w = width;
    cx = 0; cw = width; cy = 0;
    if(cw < 40) cw = 40;
    layout_node(web_root);
    flush_line();
    int page_h = cy;
    if(nabs) process_abs(0, 0, 0, width, page_h);
    pe_unsplice();
    /* positioned boxes paint over the normal flow: their commands move to the end (in order) */
    int ntop = 0;
    for(int i = 0; i < ncmds; i++) ntop += cmd_top[i];
    if(ntop && ntop < ncmds){
        web_cmd_t *g = kmalloc(sizeof(web_cmd_t) * (u32)cmd_cap);
        if(g){
            int k = 0;
            for(int i = 0; i < ncmds; i++) if(!cmd_top[i]) g[k++] = cmds[i];
            for(int i = 0; i < ncmds; i++) if(cmd_top[i]) g[k++] = cmds[i];
            kfree(cmds);
            cmds = g;
        }
    }
    return page_h;
}

const web_cmd_t *web_cmds(int *n){
    if(n) *n = ncmds;
    return cmds;
}

/* Frees the layout buffers (made again by the next web_layout). */
void web_layout_release(void){
    if(cmds) kfree(cmds);
    if(cmd_top) kfree(cmd_top);
    cmd_top = NULL;
    for(int i = 0; i < ACHUNKS; i++) if(achunk[i]){ kfree(achunk[i]); achunk[i] = NULL; }
    cmds = NULL;
    cmd_cap = 0;
    ncmds = 0;
    acur = 0; aused = 0;
}

/* The page's canvas colour, as browsers paint it behind everything: <html>'s background,
 * or <body>'s when <html> has none. 0 if the page sets neither. */
static wnode_t *child_tag(wnode_t *p, const char *tag){
    for(wnode_t *c = p ? p->first : NULL; c; c = c->next)
        if(!c->text && strcmp(c->tag, tag) == 0) return c;
    return NULL;
}
int web_canvas_bg(u32 *color){
    wnode_t *html = child_tag(web_root, "html");
    wnode_t *body = child_tag(html ? html : web_root, "body");
    if(html && html->st.bg_set){ *color = html->st.bg; return 1; }
    if(body && body->st.bg_set){ *color = body->st.bg; return 1; }
    return 0;
}
