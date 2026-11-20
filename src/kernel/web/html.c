/* HTML parser: turns the page into a tree of elements and text. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "amdroid/js.h"
#include "string.h"
int web_youtube_reader(void);

#define W_MAX_DEPTH 160
/* Nodes come from chunks made as a page needs them (a long Wikipedia article has 30,000+). */
#define POOL_CHUNK  8192
#define POOL_CHUNKS 10
static wnode_t *pool_chunks[POOL_CHUNKS];
static int pool_n = 0;
static int pool_rooted = 0;               /* the script collector scans the chunks */
u32 web_css_gen = 1;
int web_quirks = 0;

wnode_t *web_root = NULL;
char *web_css = NULL;
u32 web_css_len = 0;
char *web_js = NULL;
u32 web_js_len = 0;
u32 web_script_start[WEB_SCRIPTS_MAX], web_script_len[WEB_SCRIPTS_MAX];
int web_nscripts = 0;
/* External scripts (<script src>): the address, until the browser hands over the text.
 * Scripts run in document order once every external one has arrived. */
static char *script_src[WEB_SCRIPTS_MAX];
static u8 script_wait[WEB_SCRIPTS_MAX];   /* 1: external, not here yet */
static int scripts_deferred = 0;          /* web_load held the scripts back */

static void js_append(const char *text, u32 add, int idx){
    char *grown = kmalloc(web_js_len + add + 2);
    if(!grown) return;
    if(web_js) memcpy(grown, web_js, web_js_len);
    memcpy(grown + web_js_len, text, add);
    grown[web_js_len + add] = '\n';
    grown[web_js_len + add + 1] = 0;
    if(web_js) kfree(web_js);
    web_js = grown;
    web_script_start[idx] = web_js_len;
    web_script_len[idx] = add;
    web_js_len += add + 1;
}

int web_script_count(void){ return web_nscripts; }
const char *web_script_src(int i){ return i >= 0 && i < web_nscripts && script_wait[i] ? script_src[i] : NULL; }
void web_add_script(int i, const char *text, u32 len){
    if(i < 0 || i >= web_nscripts || !script_wait[i]) return;
    script_wait[i] = 0;
    if(len) js_append(text, len, i);
}
int web_scripts_pending(void){
    int n = 0;
    for(int i = 0; i < web_nscripts; i++) n += script_wait[i];
    return n;
}
/* Runs the page's scripts if web_load held them back for external ones. 1 if they ran. */
int web_run_scripts(void){
    if(!scripts_deferred) return 0;
    for(int i = 0; i < web_nscripts; i++) script_wait[i] = 0;   /* missing ones are skipped */
    scripts_deferred = 0;
    dom_run_scripts();
    return 1;
}
char web_title_buf[128];
char *web_link_list[WEB_LINKS_MAX];
char *web_css_ext = NULL;
u32 web_css_ext_len = 0;
#define SHEETS_MAX 48
static char *sheet_href[SHEETS_MAX];
static int nsheets = 0;

int web_stylesheet_count(void){ return nsheets; }
const char *web_stylesheet(int i){ return i >= 0 && i < nsheets ? sheet_href[i] : NULL; }

/* Fetched stylesheets, kept by their place in the page: they may arrive in any order, but
 * they cascade in document order. They count before the page's own <style> text. */
static char *sheet_text[SHEETS_MAX];
static u32 sheet_len[SHEETS_MAX];
static int sheet_next = 0;

/* Sheet i's text (NULL while it has not come). css.c parses each on its own. */
const char *web_sheet_text(int i, u32 *len){
    if(i < 0 || i >= SHEETS_MAX || !sheet_text[i]) return NULL;
    *len = sheet_len[i];
    return sheet_text[i];
}
int web_sheet_slots(void){ return SHEETS_MAX; }
u32 web_sheets_gen = 1;              /* bumped when a sheet arrives (the rest are parsed already) */

static void rebuild_ext_css(void){ web_sheets_gen++; }

void web_add_css_at(int idx, const char *css, u32 len){
    if(idx < 0 || idx >= SHEETS_MAX || sheet_text[idx]) return;
    char *c = kmalloc(len + 1);
    if(!c) return;
    memcpy(c, css, len);
    c[len] = 0;
    sheet_text[idx] = c; sheet_len[idx] = len;
    rebuild_ext_css();
}
/* Adds the next stylesheet in order. */
void web_add_css(const char *css, u32 len){
    while(sheet_next < SHEETS_MAX && sheet_text[sheet_next]) sheet_next++;
    web_add_css_at(sheet_next, css, len);
}
int web_nlinks = 0;

static char *dup_n(const char *s, u32 n){
    char *d = kmalloc(n + 1);
    if(!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

wnode_t *web_new_node(void){
    int c = pool_n / POOL_CHUNK, i = pool_n % POOL_CHUNK;
    if(c >= POOL_CHUNKS) return NULL;
    if(!pool_chunks[c]){
        pool_chunks[c] = kmalloc(sizeof(wnode_t) * POOL_CHUNK);
        if(!pool_chunks[c]) return NULL;
        memset(pool_chunks[c], 0, sizeof(wnode_t) * POOL_CHUNK);
        if(pool_rooted) js_gc_root(pool_chunks[c], sizeof(wnode_t) * POOL_CHUNK);
    }
    pool_n++;
    wnode_t *n = &pool_chunks[c][i];
    memset(n, 0, sizeof *n);
    return n;
}

/* The node pool, for the garbage collector: nodes hold pointers to script objects. */
void web_pool_roots(void){
    for(int c = 0; c < POOL_CHUNKS; c++) if(pool_chunks[c]) js_gc_root(pool_chunks[c], sizeof(wnode_t) * POOL_CHUNK);
    pool_rooted = 1;
}

void web_nodes_reset(void){
    /* clear what was used (the collector scans the chunks, old pointers must go) */
    for(int c = 0; c * POOL_CHUNK < pool_n && c < POOL_CHUNKS; c++){
        int used = pool_n - c * POOL_CHUNK;
        if(used > POOL_CHUNK) used = POOL_CHUNK;
        if(pool_chunks[c]) memset(pool_chunks[c], 0, sizeof(wnode_t) * (u32)used);
    }
    pool_n = 0;
}

static void append_child(wnode_t *parent, wnode_t *n){
    n->parent = parent;
    if(!parent->first) parent->first = n;
    else parent->last->next = n;
    parent->last = n;
}

static int lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_alpha(int c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_ws(int c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int name_eq(const char *a, const char *b){ return strcmp(a, b) == 0; }

static int is_void(const char *t){
    static const char *v[] = { "area", "base", "br", "col", "embed", "hr", "img", "input",
                               "link", "meta", "param", "source", "track", "wbr", 0 };
    for(int i = 0; v[i]; i++) if(name_eq(v[i], t)) return 1;
    return 0;
}

/* Finds "</name" (case-insensitive) from position i. Returns its index or len. */
static u32 find_end_tag(const char *html, u32 len, u32 i, const char *name){
    u32 nl = (u32)strlen(name);
    for(u32 k = i; k + 2 + nl <= len; k++){
        if(html[k] != '<' || html[k + 1] != '/') continue;
        u32 j = 0;
        while(j < nl && lower(html[k + 2 + j]) == name[j]) j++;
        if(j == nl) return k;
    }
    return len;
}

int web_entity(const char *s, u32 n, const char **out);      /* layout.c */

/* An attribute value with its character references decoded: href="a?x=1&amp;y=2" is a?x=1&y=2. */
static char *dup_attr(const char *s, u32 n){
    char *d = kmalloc(n + 1);
    if(!d) return NULL;
    u32 k = 0;
    for(u32 i = 0; i < n; ){
        const char *rep;
        int used;
        if(s[i] == '&' && (used = web_entity(s + i, n - i, &rep)) > 0){
            /* the decoded text is never longer than the reference it replaces */
            for(; *rep && k < (u32)(i + used); rep++) d[k++] = *rep;
            i += (u32)used;
        } else d[k++] = s[i++];
    }
    d[k] = 0;
    return d;
}

void wnode_attr_set(wnode_t *n, const char *name, const char *val, u32 vlen){
    for(wattr_t *a = n->attrs; a; a = a->next)
        if(name_eq(a->name, name)){ char *nv = dup_attr(val, vlen); if(nv) a->val = nv; return; }
    wattr_t *a = kmalloc(sizeof(wattr_t));
    if(!a) return;
    u32 nl = (u32)strlen(name);
    a->name = kmalloc(nl + 1);
    if(a->name) memcpy(a->name, name, nl + 1);
    a->val = dup_attr(val, vlen);
    a->next = n->attrs;
    n->attrs = a;
}
const char *wnode_attr_get(wnode_t *n, const char *name){
    for(wattr_t *a = n->attrs; a; a = a->next)
        if(name_eq(a->name, name)) return a->val;
    return NULL;
}

static void set_attr(wnode_t *n, const char *name, const char *val, u32 vlen){
    if(name_eq(name, "id")) n->id = dup_attr(val, vlen);
    else if(name_eq(name, "class")) n->cls = dup_attr(val, vlen);
    else if(name_eq(name, "href")) n->href = dup_attr(val, vlen);
    else if(name_eq(name, "style")) n->style = dup_attr(val, vlen);
    else if(name_eq(name, "alt")) n->alt = dup_attr(val, vlen);
    else if(name_eq(name, "onclick")) n->onclick = dup_attr(val, vlen);
    else if(name_eq(name, "value")) n->value = dup_attr(val, vlen);
    else if(name_eq(name, "placeholder")) n->alt = dup_attr(val, vlen);
    else if(name_eq(name, "hidden")){ if(!(vlen == 11 && lower(val[0]) == 'u')) n->hidden = 1; }   /* not hidden="until-found" */
    else if(name_eq(n->tag, "link") && name_eq(name, "rel")){
        /* "stylesheet" among the words, and not an alternate sheet */
        int has = 0, alt = 0;
        for(u32 k = 0; k + 10 <= vlen; k++){
            u32 j = 0;
            while(j < 10 && lower(val[k + j]) == "stylesheet"[j]) j++;
            if(j == 10) has = 1;
        }
        for(u32 k = 0; k + 9 <= vlen; k++){
            u32 j = 0;
            while(j < 9 && lower(val[k + j]) == "alternate"[j]) j++;
            if(j == 9) alt = 1;
        }
        if(has && !alt) n->css_link |= 1;
    }
    else if(name_eq(n->tag, "link") && name_eq(name, "media")){
        if(vlen >= 5 && lower(val[0]) == 'p' && lower(val[1]) == 'r' && lower(val[2]) == 'i') n->css_link |= 2;
    }
    else if(name_eq(name, "type") && name_eq(n->tag, "script")){
        /* only classic JavaScript runs; JSON data, templates and modules do not */
        static const char *js_types[] = { "text/javascript", "application/javascript", "text/ecmascript",
                                          "application/ecmascript", "", 0 };
        int ok = 0;
        for(int k = 0; js_types[k]; k++){
            u32 tl = (u32)strlen(js_types[k]);
            if(tl != vlen) continue;
            u32 j = 0;
            while(j < vlen && lower(val[j]) == js_types[k][j]) j++;
            if(j == vlen){ ok = 1; break; }
        }
        n->skip = !ok;
    }
    else wnode_attr_set(n, name, val, vlen);     /* data-*, type, name, src, ... */
}

/* Reads a tag starting after '<' at i. Fills name and attributes; returns the index after '>'. */
static u32 read_tag(const char *html, u32 len, u32 i, wnode_t *n, int *self_close, int *closing){
    *self_close = 0;
    *closing = 0;
    if(i < len && html[i] == '/'){ *closing = 1; i++; }
    u32 k = 0;
    while(i < len && !is_ws(html[i]) && html[i] != '>' && html[i] != '/' && k + 1 < sizeof n->tag)
        n->tag[k++] = (char)lower(html[i++]);
    n->tag[k] = 0;
    while(i < len && html[i] != '>'){
        if(html[i] == '/'){ *self_close = 1; i++; continue; }
        if(is_ws(html[i])){ i++; continue; }
        char an[64]; u32 an_n = 0;
        while(i < len && !is_ws(html[i]) && html[i] != '=' && html[i] != '>' && html[i] != '/'){
            if(an_n + 1 < sizeof an) an[an_n++] = (char)lower(html[i]);
            i++;                                     /* a longer name is cut, not split in two */
        }
        an[an_n] = 0;
        while(i < len && is_ws(html[i])) i++;
        if(i < len && html[i] == '='){
            i++;
            while(i < len && is_ws(html[i])) i++;
            u32 vs, ve;
            if(i < len && (html[i] == '"' || html[i] == '\'')){
                char q = html[i++];
                vs = i;
                while(i < len && html[i] != q) i++;
                ve = i;
                if(i < len) i++;
            } else {
                vs = i;
                while(i < len && !is_ws(html[i]) && html[i] != '>') i++;
                ve = i;
            }
            if(an_n) set_attr(n, an, html + vs, ve - vs);
        } else if(an_n) set_attr(n, an, "", 0);         /* a bare attribute: <div hidden> */
    }
    if(i < len) i++;   /* the '>' */
    return i;
}

const char *web_uni_ascii(u32 cp);          /* layout.c */
int web_utf8(const char *s, u32 n, u32 *cp);
int web_entity(const char *s, u32 n, const char **out);

static void add_title(const char *s, u32 len){
    u32 t = (u32)strlen(web_title_buf);
    int pending = 0;
    for(u32 k = 0; k < len; ){
        char c = s[k];
        if(is_ws(c)){ pending = t > 0; k++; continue; }
        const char *rep;
        char one[2] = { c, 0 };
        int used;
        if((unsigned char)c >= 0x80){ u32 cp; k += (u32)web_utf8(s + k, len - k, &cp); rep = web_uni_ascii(cp); }
        else if(c == '&' && (used = web_entity(s + k, len - k, &rep)) > 0) k += (u32)used;
        else { rep = one; k++; }
        if(!*rep) continue;
        if(pending && t + 2 < sizeof web_title_buf){ web_title_buf[t++] = ' '; pending = 0; }
        for(; *rep; rep++) if(t + 1 < sizeof web_title_buf) web_title_buf[t++] = *rep;
    }
    web_title_buf[t] = 0;
}

/* The HTML tokenizer and tree builder. Appends the parsed nodes under root.
 * When fragment is set (innerHTML), <title>/<style>/<script>/<link> are not
 * collected or executed — only element and text nodes are built. */
static int saw_doctype = 0;

static int in_list(const char *t, const char *const *l){
    for(int i = 0; l[i]; i++) if(name_eq(t, l[i])) return 1;
    return 0;
}
/* Start tags that close an open <p> (HTML's "closes a p element"). */
static const char *const closes_p[] = { "address", "article", "aside", "blockquote", "center", "details", "dialog",
    "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
    "header", "hgroup", "hr", "main", "menu", "nav", "ol", "p", "pre", "section", "summary", "table", "ul",
    "listing", "xmp", "plaintext", "search", 0 };
/* Where looking for an open element to close stops. */
static const char *const scope_stop[] = { "table", "td", "th", "caption", "html", "#root", "template", "object",
    "marquee", "applet", "button", 0 };

/* Index of the nearest open `tag` above the scope boundary, or -1. */
static int open_in_scope(wnode_t **stack, int sp, const char *tag, const char *const *extra_stop){
    for(int k = sp - 1; k > 0; k--){
        if(name_eq(stack[k]->tag, tag)) return k;
        if(in_list(stack[k]->tag, scope_stop)) return -1;
        if(extra_stop && in_list(stack[k]->tag, extra_stop)) return -1;
    }
    return -1;
}

static void parse_into(const char *html, u32 len, wnode_t *root, int fragment){
    wnode_t *stack[W_MAX_DEPTH];
    int sp = 0;
    stack[sp++] = root;

    u32 i = 0;
    while(i < len){
        if(html[i] != '<'){
            u32 e = i;
            while(e < len && html[e] != '<') e++;
            wnode_t *t = web_new_node();
            if(t){
                t->text = 1;
                t->txt = dup_n(html + i, e - i);
                t->tlen = e - i;
                append_child(stack[sp - 1], t);
            }
            i = e;
            continue;
        }
        /* comments and declarations */
        if(i + 3 < len && html[i+1] == '!' && html[i+2] == '-' && html[i+3] == '-'){
            u32 k = i + 4;
            while(k + 2 < len && !(html[k] == '-' && html[k+1] == '-' && html[k+2] == '>')) k++;
            i = k + 3;
            continue;
        }
        if(i + 1 < len && (html[i+1] == '!' || html[i+1] == '?')){
            /* <!DOCTYPE html> is a standards-mode page; anything else (or none) is quirks */
            if(!fragment && i + 9 < len && lower(html[i+2]) == 'd' && lower(html[i+3]) == 'o' && lower(html[i+4]) == 'c'){
                u32 k = i + 9;
                while(k < len && is_ws(html[k])) k++;
                int html5 = k + 4 <= len && lower(html[k]) == 'h' && lower(html[k+1]) == 't' && lower(html[k+2]) == 'm' && lower(html[k+3]) == 'l';
                int strictish = 0;
                for(u32 q = k; q + 6 < len && html[q] != '>'; q++)
                    if((html[q] == 'S' || html[q] == 's') && (html[q+1] == 't' || html[q+1] == 'T') && (html[q+2] == 'r' || html[q+2] == 'R')) strictish = 1;
                for(u32 q = k; q + 4 < len && html[q] != '>'; q++)
                    if(html[q] == 'h' && html[q+1] == 't' && html[q+2] == 't' && html[q+3] == 'p') strictish = 1;   /* has a system id */
                web_quirks = !(html5 || strictish);
                saw_doctype = 1;
            }
            while(i < len && html[i] != '>') i++;
            i++;
            continue;
        }
        if(i + 1 >= len || (!is_alpha(html[i+1]) && html[i+1] != '/')){
            /* a lone '<' is text */
            wnode_t *t = web_new_node();
            if(t){ t->text = 1; t->txt = dup_n("<", 1); t->tlen = 1; append_child(stack[sp - 1], t); }
            i++;
            continue;
        }
        wnode_t *n = web_new_node();
        if(!n){ i = len; break; }
        int self_close = 0, closing = 0;
        i = read_tag(html, len, i + 1, n, &self_close, &closing);

        if(!fragment && !closing && name_eq(n->tag, "link") && n->css_link == 1 && n->href && nsheets < SHEETS_MAX && !web_youtube_reader()){
            int dup = 0;                                     /* the same sheet linked twice counts once */
            for(int k = 0; k < nsheets; k++) if(strcmp(sheet_href[k], n->href) == 0) dup = 1;
            u32 hl = (u32)strlen(n->href);
            char *h = dup ? NULL : kmalloc(hl + 1);
            if(h){ memcpy(h, n->href, hl + 1); sheet_href[nsheets++] = h; }
        }
        if(!closing && name_eq(n->tag, "img")){                 /* also innerHTML: scripts add images */
            const char *src = wnode_attr_get(n, "src");
            /* lazy loading: the real address waits in data-src while src is a placeholder */
            const char *lazy = wnode_attr_get(n, "data-src");
            if(!lazy) lazy = wnode_attr_get(n, "data-lazy-src");
            if(!lazy) lazy = wnode_attr_get(n, "data-original");
            if(lazy && lazy[0] && (!src || !src[0] || (src[0] == 'd' && src[1] == 'a' && src[2] == 't' && src[3] == 'a' && src[4] == ':'))) src = lazy;
            if((!src || !src[0])){
                /* only a srcset: its first candidate */
                const char *ss = wnode_attr_get(n, "srcset");
                if(!ss) ss = wnode_attr_get(n, "data-srcset");
                if(ss && ss[0]){
                    char first[512]; u32 k = 0;
                    while(*ss == ' ') ss++;
                    while(ss[k] && ss[k] != ' ' && ss[k] != ',' && k + 1 < sizeof first){ first[k] = ss[k]; k++; }
                    first[k] = 0;
                    if(k) n->img = img_register(first) + 1;
                    src = NULL;
                }
            }
            if(src && src[0]) n->img = img_register(src) + 1;   /* 0 when the table is full */
        }
        if(closing){
            /* </p> and </li> only close inside their scope; others close the nearest match,
               but never past the table or cell they are in */
            static const char *const cell_stop[] = { "td", "th", "table", 0 };
            int is_cellish = name_eq(n->tag, "td") || name_eq(n->tag, "th") || name_eq(n->tag, "tr") ||
                             name_eq(n->tag, "table") || name_eq(n->tag, "tbody") || name_eq(n->tag, "thead") || name_eq(n->tag, "tfoot");
            for(int k = sp - 1; k > 0; k--){
                if(name_eq(stack[k]->tag, n->tag)){ sp = k; break; }
                if(!is_cellish && in_list(stack[k]->tag, cell_stop)) break;
                if(name_eq(n->tag, "td") || name_eq(n->tag, "th")){ if(name_eq(stack[k]->tag, "table")) break; }
            }
            continue;
        }
        /* implied end tags */
        if(sp > 1){
            const char *t = n->tag;
            if(in_list(t, closes_p)){
                int k = open_in_scope(stack, sp, "p", NULL);
                if(k > 0) sp = k;
            }
            if(name_eq(t, "li")){
                static const char *const li_stop[] = { "ul", "ol", "menu", 0 };
                int k = open_in_scope(stack, sp, "li", li_stop);
                if(k > 0) sp = k;
            } else if(name_eq(t, "dt") || name_eq(t, "dd")){
                static const char *const d_stop[] = { "dl", 0 };
                int k = open_in_scope(stack, sp, "dd", d_stop), k2 = open_in_scope(stack, sp, "dt", d_stop);
                if(k2 > k) k = k2;
                if(k > 0) sp = k;
            } else if(name_eq(t, "option") || name_eq(t, "optgroup")){
                if(name_eq(stack[sp-1]->tag, "option")) sp--;
                if(name_eq(t, "optgroup") && sp > 1 && name_eq(stack[sp-1]->tag, "optgroup")) sp--;
            } else if(name_eq(t, "td") || name_eq(t, "th")){
                /* close the open cell of this row (and anything left open inside it) */
                for(int k = sp - 1; k > 0; k--){
                    if(name_eq(stack[k]->tag, "td") || name_eq(stack[k]->tag, "th")){ sp = k; break; }
                    if(name_eq(stack[k]->tag, "tr") || name_eq(stack[k]->tag, "table")) break;
                }
            } else if(name_eq(t, "tr")){
                for(int k = sp - 1; k > 0; k--){
                    if(name_eq(stack[k]->tag, "tr")){ sp = k; break; }
                    if(name_eq(stack[k]->tag, "table")) break;
                }
            } else if(name_eq(t, "tbody") || name_eq(t, "thead") || name_eq(t, "tfoot")){
                for(int k = sp - 1; k > 0; k--){
                    if(name_eq(stack[k]->tag, "tbody") || name_eq(stack[k]->tag, "thead") || name_eq(stack[k]->tag, "tfoot")){ sp = k; break; }
                    if(name_eq(stack[k]->tag, "table")) break;
                }
            } else if(name_eq(t, "a")){
                /* <a> inside an open <a> closes it */
                for(int k = sp - 1; k > 0; k--){
                    if(name_eq(stack[k]->tag, "a")){ sp = k; break; }
                    if(in_list(stack[k]->tag, scope_stop)) break;
                }
            }
            /* a body or html tag that comes late: its attributes go to the one we have */
        }
        if(name_eq(n->tag, "textarea") && !self_close){
            /* raw text: the content is the box's text, not markup */
            u32 end = find_end_tag(html, len, i, "textarea");
            if(sp < W_MAX_DEPTH) append_child(stack[sp - 1], n);
            wnode_t *t = web_new_node();
            if(t && end > i){ t->text = 1; t->txt = dup_n(html + i, end - i); t->tlen = end - i; append_child(n, t); }
            i = end;
            while(i < len && html[i] != '>') i++;
            i++;
            continue;
        }

        if(name_eq(n->tag, "title") || name_eq(n->tag, "style") || name_eq(n->tag, "script")){
            u32 end = find_end_tag(html, len, i, n->tag);
            if(fragment){ i = end; while(i < len && html[i] != '>') i++; i++; continue; }  /* skip raw content */
            if(name_eq(n->tag, "title")){
                if(!web_title_buf[0]) add_title(html + i, end - i);    /* the first one (an <svg> can have its own) */
            } else if(name_eq(n->tag, "style")){
                u32 add = end - i;
                char *grown = kmalloc(web_css_len + add + 2);
                if(grown){
                    if(web_css) memcpy(grown, web_css, web_css_len);
                    memcpy(grown + web_css_len, html + i, add);
                    grown[web_css_len + add] = '\n';
                    grown[web_css_len + add + 1] = 0;
                    if(web_css) kfree(web_css);
                    web_css = grown;
                    web_css_len += add + 1;
                    web_css_gen++;
                }
            }
            if(name_eq(n->tag, "script") && !n->skip && web_nscripts < WEB_SCRIPTS_MAX &&
               !(web_youtube_reader() && wnode_attr_get(n, "src"))){   /* the reader needs no app scripts */
                const char *src = wnode_attr_get(n, "src");
                int idx = web_nscripts++;
                web_script_start[idx] = 0; web_script_len[idx] = 0;
                if(src && src[0]){
                    u32 sl = (u32)strlen(src);
                    script_src[idx] = kmalloc(sl + 1);
                    if(script_src[idx]){ memcpy(script_src[idx], src, sl + 1); script_wait[idx] = 1; }
                } else {
                    script_src[idx] = NULL; script_wait[idx] = 0;
                    js_append(html + i, end - i, idx);
                }
            }
            i = end;
            while(i < len && html[i] != '>') i++;
            i++;
            continue;
        }

        if(sp < W_MAX_DEPTH){
            append_child(stack[sp - 1], n);
            if(!is_void(n->tag) && !self_close) stack[sp++] = n;
        }
    }
}

int web_load(const char *html, u32 len){
    web_free();
    web_nodes_reset();
    web_quirks = 1;
    saw_doctype = 0;
    web_css_gen++;
    web_root = web_new_node();
    if(!web_root) return -1;
    web_root->text = 0;
    strcpy(web_root->tag, "#root");
    web_root->st.display = W_BLOCK;

    parse_into(html, len, web_root, 0);
    if(web_scripts_pending()) scripts_deferred = 1;     /* the browser fetches them, then web_run_scripts */
    else dom_run_scripts();
    return 0;
}

/* Parses an HTML fragment and appends the resulting nodes under parent
 * (for element.innerHTML = "<...>"). Scripts and stylesheets are not run. */
void web_parse_fragment(const char *html, u32 len, wnode_t *parent){
    if(parent) parse_into(html, len, parent, 1);
}

void web_free(void){
    for(int i = 0; i < web_nlinks; i++){ if(web_link_list[i]) kfree(web_link_list[i]); web_link_list[i] = NULL; }
    web_nlinks = 0;
    if(web_css){ kfree(web_css); web_css = NULL; }
    web_css_len = 0;
    if(web_css_ext){ kfree(web_css_ext); web_css_ext = NULL; }
    web_css_ext_len = 0;
    web_css_gen++;
    for(int i = 0; i < SHEETS_MAX; i++){ if(sheet_text[i]) kfree(sheet_text[i]); sheet_text[i] = NULL; sheet_len[i] = 0; }
    sheet_next = 0;
    for(int i = 0; i < nsheets; i++){ kfree(sheet_href[i]); sheet_href[i] = NULL; }
    nsheets = 0;
    web_images_reset();
    if(web_js){ kfree(web_js); web_js = NULL; }
    web_js_len = 0;
    for(int i = 0; i < web_nscripts; i++){ if(script_src[i]) kfree(script_src[i]); script_src[i] = NULL; script_wait[i] = 0; }
    web_nscripts = 0;
    scripts_deferred = 0;
    dom_reset();
    web_title_buf[0] = 0;
    web_root = NULL;
    web_nodes_reset();
}

const char *web_title(void){ return web_title_buf; }

const char *web_link(int idx){
    if(idx < 0 || idx >= web_nlinks) return NULL;
    return web_link_list[idx];
}

void web_layout_release(void);
/* Frees everything: the page, its layout and the script heap. */
void web_release(void){
    web_free();
    web_layout_release();
    js_release();
}
