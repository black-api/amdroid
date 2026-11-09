/* HTML parser: turns the page into a tree of elements and text. */
#include "amdroid/web.h"
#include "amdroid/heap.h"
#include "web_internal.h"
#include "amdroid/js.h"
#include "string.h"

#define W_MAX_NODES 10000
#define W_MAX_DEPTH 96

static wnode_t pool[W_MAX_NODES];
static int pool_n = 0;

wnode_t *web_root = NULL;
char *web_css = NULL;
u32 web_css_len = 0;
char *web_js = NULL;
u32 web_js_len = 0;
u32 web_script_start[WEB_SCRIPTS_MAX], web_script_len[WEB_SCRIPTS_MAX];
int web_nscripts = 0;
char web_title_buf[128];
char *web_link_list[WEB_LINKS_MAX];
char *web_css_ext = NULL;
u32 web_css_ext_len = 0;
#define SHEETS_MAX 12
static char *sheet_href[SHEETS_MAX];
static int nsheets = 0;

int web_stylesheet_count(void){ return nsheets; }
const char *web_stylesheet(int i){ return i >= 0 && i < nsheets ? sheet_href[i] : NULL; }

/* Adds a fetched stylesheet; it counts before the page's own <style> text. */
void web_add_css(const char *css, u32 len){
    char *grown = kmalloc(web_css_ext_len + len + 2);
    if(!grown) return;
    if(web_css_ext) memcpy(grown, web_css_ext, web_css_ext_len);
    memcpy(grown + web_css_ext_len, css, len);
    grown[web_css_ext_len + len] = '\n';
    grown[web_css_ext_len + len + 1] = 0;
    if(web_css_ext) kfree(web_css_ext);
    web_css_ext = grown;
    web_css_ext_len += len + 1;
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
    if(pool_n >= W_MAX_NODES) return NULL;
    wnode_t *n = &pool[pool_n++];
    memset(n, 0, sizeof *n);
    return n;
}

/* The node pool, for the garbage collector: it holds pointers to script objects. */
void web_pool_range(void **p, u32 *n){ *p = pool; *n = sizeof pool; }

void web_nodes_reset(void){
    pool_n = 0;
    memset(pool, 0, sizeof pool);
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
        char an[24]; u32 an_n = 0;
        while(i < len && !is_ws(html[i]) && html[i] != '=' && html[i] != '>' && html[i] != '/' && an_n + 1 < sizeof an)
            an[an_n++] = (char)lower(html[i++]);
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

int web_load(const char *html, u32 len){
    web_free();
    web_nodes_reset();
    web_root = web_new_node();
    if(!web_root) return -1;
    web_root->text = 0;
    strcpy(web_root->tag, "#root");
    web_root->st.display = W_BLOCK;

    wnode_t *stack[W_MAX_DEPTH];
    int sp = 0;
    stack[sp++] = web_root;

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

        if(!closing && name_eq(n->tag, "link") && n->css_link == 1 && n->href && nsheets < SHEETS_MAX){
            u32 hl = (u32)strlen(n->href);
            char *h = kmalloc(hl + 1);
            if(h){ memcpy(h, n->href, hl + 1); sheet_href[nsheets++] = h; }
        }
        if(closing){
            for(int k = sp - 1; k > 0; k--)
                if(name_eq(stack[k]->tag, n->tag)){ sp = k; break; }
            continue;
        }
        /* implied end tags: a new <p> or <li> closes the open one */
        if(sp > 1 && ((name_eq(n->tag, "p") && name_eq(stack[sp-1]->tag, "p")) ||
                      (name_eq(n->tag, "li") && name_eq(stack[sp-1]->tag, "li")) ||
                      (name_eq(n->tag, "tr") && name_eq(stack[sp-1]->tag, "tr")) ||
                      (name_eq(n->tag, "td") && (name_eq(stack[sp-1]->tag, "td") || name_eq(stack[sp-1]->tag, "th")))))
            sp--;

        if(name_eq(n->tag, "title") || name_eq(n->tag, "style") || name_eq(n->tag, "script")){
            u32 end = find_end_tag(html, len, i, n->tag);
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
                }
            }
            if(name_eq(n->tag, "script") && !n->skip){
                u32 add = end - i;
                if(web_nscripts < WEB_SCRIPTS_MAX){ web_script_start[web_nscripts] = web_js_len; web_script_len[web_nscripts] = add; web_nscripts++; }
                char *grown = kmalloc(web_js_len + add + 2);
                if(grown){
                    if(web_js) memcpy(grown, web_js, web_js_len);
                    memcpy(grown + web_js_len, html + i, add);
                    grown[web_js_len + add] = '\n';
                    grown[web_js_len + add + 1] = 0;
                    if(web_js) kfree(web_js);
                    web_js = grown;
                    web_js_len += add + 1;
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
    dom_run_scripts();
    return 0;
}

void web_free(void){
    for(int i = 0; i < web_nlinks; i++){ if(web_link_list[i]) kfree(web_link_list[i]); web_link_list[i] = NULL; }
    web_nlinks = 0;
    if(web_css){ kfree(web_css); web_css = NULL; }
    web_css_len = 0;
    if(web_css_ext){ kfree(web_css_ext); web_css_ext = NULL; }
    web_css_ext_len = 0;
    for(int i = 0; i < nsheets; i++){ kfree(sheet_href[i]); sheet_href[i] = NULL; }
    nsheets = 0;
    if(web_js){ kfree(web_js); web_js = NULL; }
    web_js_len = 0;
    web_nscripts = 0;
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
