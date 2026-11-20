/* Browser: a web browser. It loads pages over HTTP (and HTTPS through the host's proxy,
 * tools/https_proxy.py). The page itself — HTML, CSS, JavaScript, painting — is rendered by
 * Husser, a separate ring-3 process with its own address space; this app owns the network,
 * the window chrome, and what to do when Husser asks for something.
 *
 * Loading runs a step at a time from the draw loop, so the desktop keeps moving while a
 * page arrives: a progress bar runs under the toolbar, Go turns into Stop, and the footer
 * says what is happening. A page that cannot be loaded gets an error page with Try again. */
#include "amdroid/ui.h"
#include "amdroid/net.h"
#include "amdroid/heap.h"
#include "amdroid/keyboard.h"
#include "amdroid/husser.h"
#include "amdroid/reel.h"
#include "amdroid/fb.h"
#include "amdroid/timer.h"
#include "amdroid/serial.h"
#include "amdroid/markdown.h"
#include "amdroid/fat32.h"
#include "string.h"

#define URL_MAX     1024           /* stylesheet addresses can be long (Wikipedia: ~500) */
#define HOST_MAX    64
#define PATH_MAX_B  1024
#define HIST_MAX    16
#define BODY_MAX    (16 * 1024 * 1024)     /* big pages and scripts (YouTube's app is ~11 MB) */
#define LOAD_MAX    (ultra_mode() ? 96u * 1024 * 1024 : 32u * 1024 * 1024)   /* a download, at most */
#define SCROLL_STEP 48
#define MAX_REDIRECTS 5
#define FIRST_BYTE_TICKS 3000     /* 30 s for the first byte (the proxy may be fetching) */
#define IDLE_TICKS       1000     /* 10 s of silence after that ends the page */
#define RES_FAIL_LEN     0xFFFFFFFFu

#define PROXY_IP   0x0A000202u    /* 10.0.2.2: the host, as seen from the guest */
#define PROXY_PORT 8888

/* Browser settings (the Settings app switches them). */
int browser_adblock = 1;            /* requests to ad and tracker servers are not made */
int browser_sort_downloads = 1;     /* downloads go into PICTURES, VIDEOS, ... under HOME/DOWNLOAD */
static int ads_blocked = 0;         /* this page */

static char addr[URL_MAX];
static int addr_len = 0;
static int editing = 0;
static char cur_url[URL_MAX];
static char status[128] = "";

static char hist[HIST_MAX][URL_MAX];
static int hist_scroll[HIST_MAX];   /* where each earlier page was scrolled to */
static int nhist = 0;
static char num_buf[4];
static int num_len = 0;
static int scroll = 0;              /* pixels */
static int have_page = 0;
static rect_t last_body;            /* from the last draw; w is 0 until the window has been drawn */
static int hover_btn = -1;          /* toolbar button under the pointer */
static int last_hx = -1, last_hy = -1;   /* the pointer position Husser was last told */
/* husser protocol bookkeeping */
static u32 need_seq_consumed = 0;   /* the need list we last saw */
static u32 nav_seq_consumed = 0;    /* a navigation Husser asked for */
#define RES_KINDS 3                  /* 1 stylesheet, 2 image, 3 script */
#define RES_IDX   256
static u8  res_done[RES_KINDS][RES_IDX];   /* [kind-1][idx]: fetched or in flight */
static int waiting_link = -1;       /* the link index we are waiting a url for */
static u32 waiting_link_seq = 0;    /* the link_url_seq it answers */
extern const app_t app_browser;

/* A notification when the user is looking at something else. */
static void tell(const char *title, const char *body){
    int me = ui_app_index(&app_browser);
    if(!ui_app_is_front(me)) ui_notify(me, title, body);
}

/* ---- small string helpers ---- */

static int starts(const char *s, const char *p){
    while(*p) if(*s++ != *p++) return 0;
    return 1;
}
static int lower(int c){ return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static void cat_n(char *d, const char *s, u32 max){
    u32 n = ui_len(d);
    while(*s && n + 1 < max) d[n++] = *s++;
    d[n] = 0;
}
static int same(const char *a, const char *b){
    while(*a && *a == *b){ a++; b++; }
    return *a == *b;
}

/* ---- URLs ---- */

typedef struct { int https; char host[HOST_MAX]; u16 port; char path[PATH_MAX_B]; } url_t;

/* Reads "http[s]://host[:port][/path][?query][#fragment]". Returns 0 on success. */
static int parse_url(const char *u, url_t *o){
    o->https = 0;
    if(starts(u, "https://")){ o->https = 1; u += 8; }
    else if(starts(u, "http://")) u += 7;
    else return -1;
    u32 n = 0;
    while(*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && n + 1 < HOST_MAX) o->host[n++] = (char)lower(*u++);
    o->host[n] = 0;
    if(n == 0) return -1;
    o->port = o->https ? 443 : 80;
    if(*u == ':'){
        u++;
        unsigned v = 0; int digits = 0;
        while(*u >= '0' && *u <= '9'){ v = v * 10 + (unsigned)(*u - '0'); u++; digits++; }
        if(!digits || v == 0 || v > 65535) return -1;
        o->port = (u16)v;
    }
    o->path[0] = 0;
    if(*u != '/') ui_copy(o->path, "/", PATH_MAX_B);
    cat_n(o->path, u, PATH_MAX_B);
    for(char *q = o->path; *q; q++) if(*q == '#'){ *q = 0; break; }   /* the fragment is not sent */
    return 0;
}

/* "https://host[:port]" */
static void url_origin(const url_t *o, char *out, u32 max){
    ui_copy(out, o->https ? "https://" : "http://", max);
    cat_n(out, o->host, max);
    if(o->port != (o->https ? 443 : 80)){
        char n[8] = "";
        ui_num(o->port, n);
        cat_n(out, ":", max);
        cat_n(out, n, max);
    }
}

/* Removes "." and ".." segments from an absolute path (the query is kept as it is). */
static void normalize(char *p, u32 max){
    char tail[PATH_MAX_B];
    u32 i = 0;
    while(p[i] && p[i] != '?') i++;
    ui_copy(tail, p + i, sizeof tail);
    p[i] = 0;
    char out[PATH_MAX_B];
    u32 o = 0;
    out[o++] = '/';
    const char *s = p;
    if(*s == '/') s++;
    while(*s){
        const char *e = s;
        while(*e && *e != '/') e++;
        u32 len = (u32)(e - s);
        int last = *e == 0;
        if(len == 1 && s[0] == '.'){ }
        else if(len == 2 && s[0] == '.' && s[1] == '.'){
            if(o > 1){ o--; while(o > 1 && out[o - 1] != '/') o--; }
        } else if(len){
            for(u32 k = 0; k < len && o + 2 < sizeof out; k++) out[o++] = s[k];
            if(!last && o + 1 < sizeof out) out[o++] = '/';
        }
        s = last ? e : e + 1;
    }
    out[o] = 0;
    ui_copy(p, out, max);
    cat_n(p, tail, max);
}

/* The full URL for href on the page at base. */
static void resolve(const char *base, const char *href, char *out, u32 max){
    while(*href == ' ' || *href == '\n' || *href == '\t') href++;
    if(starts(href, "http://") || starts(href, "https://")){ ui_copy(out, href, max); return; }
    url_t b;
    if(parse_url(base, &b) != 0){ ui_copy(out, href, max); return; }
    if(href[0] == '/' && href[1] == '/'){            /* same scheme, another host */
        ui_copy(out, b.https ? "https:" : "http:", max);
        cat_n(out, href, max);
        return;
    }
    char path[PATH_MAX_B];
    if(href[0] == 0 || href[0] == '#') ui_copy(path, b.path, sizeof path);
    else if(href[0] == '/') ui_copy(path, href, sizeof path);
    else if(href[0] == '?'){
        u32 i = 0;
        while(b.path[i] && b.path[i] != '?' && i + 1 < sizeof path){ path[i] = b.path[i]; i++; }
        path[i] = 0;
        cat_n(path, href, sizeof path);
    } else {
        /* relative to the folder of the current page */
        u32 cut = 0;
        for(u32 i = 0; b.path[i] && b.path[i] != '?'; i++) if(b.path[i] == '/') cut = i + 1;
        u32 i;
        for(i = 0; i < cut && i + 1 < sizeof path; i++) path[i] = b.path[i];
        path[i] = 0;
        cat_n(path, href, sizeof path);
    }
    for(char *q = path; *q; q++) if(*q == '#'){ *q = 0; break; }
    normalize(path, sizeof path);
    url_origin(&b, out, max);
    cat_n(out, path, max);
}

/* What the user typed, as a full URL: "example.com" becomes "http://example.com/". */
static void typed_url(const char *in, char *out, u32 max){
    while(*in == ' ') in++;
    if(starts(in, "http://") || starts(in, "https://") || starts(in, "about:")){ ui_copy(out, in, max); return; }
    /* words, not an address: search for them (DuckDuckGo's page for simple browsers works
       without scripts) */
    int space = 0, dot = 0, colon = 0;
    for(const char *q = in; *q; q++){ if(*q == ' ') space = 1; if(*q == '.') dot = 1; if(*q == ':') colon = 1; }
    if(space || (!dot && !colon && !starts(in, "localhost"))){
        static const char hex[] = "0123456789ABCDEF";
        ui_copy(out, "https://html.duckduckgo.com/html/?q=", max);
        u32 k = ui_len(out);
        for(const char *q = in; *q && k + 4 < max; q++){
            unsigned char c = (unsigned char)*q;
            if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[k++] = (char)c;
            else if(c == ' ') out[k++] = '+';
            else { out[k++] = '%'; out[k++] = hex[c >> 4]; out[k++] = hex[c & 15]; }
        }
        out[k] = 0;
        return;
    }
    ui_copy(out, "http://", max);
    cat_n(out, in, max);
}

/* ---- pages made here: the start page and error pages ---- */

static void esc_cat(char *d, const char *s, u32 max){
    for(; *s; s++){
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : 0;
        if(r) cat_n(d, r, max);
        else { char c[2] = { *s, 0 }; cat_n(d, c, max); }
    }
}

static const char start_page[] =
    "<!DOCTYPE html><html><head><title>New tab</title><style>"
    "html{background:#1c2328}body{margin:0;color:#c9d4dc}"
    ".hero{background:rgb(23,42,56);padding:22px 24px}"
    "h1{color:#4cc2ff;margin:0}"
    ".sub{color:#9aa7b0;margin:6px 0 0 0}"
    ".wrap{padding:8px 24px 24px 24px}"
    "ul{margin:4px 0}li{margin:10px 0}"
    "a{color:#4cc2ff}"
    ".http a{color:rgb(61,220,151)}"
    ".secure a{color:#7c6cff}"
    ".news a{color:#ffb547}"
    ".ref a{color:#ff6b9d}"
    ".tag{color:#63717b}"
    ".keys{background:rgb(21,32,41);padding:14px 24px;color:#9aa7b0}"
    ".keys b{color:#ff8c42}"
    "</style></head><body>"
    "<div class=\"hero\"><h1>Browser</h1>"
    "<p class=\"sub\">Type an address (or words to search) in the bar above and press Enter, or open one of these:</p></div>"
    "<div class=\"wrap\"><ul>"
    "<li class=\"http\"><a href=\"http://example.com/\">example.com</a> <span class=\"tag\">- a small test page (HTTP)</span></li>"
    "<li class=\"secure\"><a href=\"https://example.com/\">example.com over HTTPS</a> <span class=\"tag\">- through the host proxy</span></li>"
    "<li class=\"ref\"><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first website</a> <span class=\"tag\">- CERN, 1991</span></li>"
    "<li class=\"news\"><a href=\"https://text.npr.org/\">text.npr.org</a> <span class=\"tag\">- news in plain HTML</span></li>"
    "<li class=\"news\"><a href=\"https://lite.cnn.com/\">lite.cnn.com</a> <span class=\"tag\">- CNN, text only</span></li>"
    "<li class=\"news\"><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
    "<li class=\"secure\"><a href=\"https://lobste.rs/\">Lobsters</a> <span class=\"tag\">- with its stylesheets</span></li>"
    "<li class=\"ref\"><a href=\"https://en.wikipedia.org/wiki/Linux\">Wikipedia: Linux</a> <span class=\"tag\">- a 1 MB page</span></li>"
    "</ul></div>"
    "<div class=\"keys\"><b>Keys:</b> type a link number and press Enter to follow it; Backspace goes back; "
    "arrows and Page Up/Down scroll; Esc stops a page that is loading.</div>"
    "</body></html>";

/* ---- loading ---- */

enum { LD_IDLE, LD_START, LD_CONNECT, LD_RECV };
static int ld = LD_IDLE;
static char ld_url[URL_MAX];          /* what is being loaded */
static int ld_remember;               /* put the current page into history when it arrives */
static int ld_scroll;                 /* scroll to this when it arrives (-1: top) */
static int ld_redirects;
static int ld_sock = -1;
static u8 *ld_buf;
static u32 ld_len, ld_hdr, ld_clen, ld_cap;
static int ld_have_clen;
static u64 ld_t0, ld_tlast;
static char ld_host[HOST_MAX];        /* shown in the footer */
static int ld_kind;                   /* 0 the page, 1 one of its stylesheets, 2 one of its images */
static int css_done;                  /* for the final "Loaded" line */
static int img_done;
static u32 page_kb;                   /* for the final "Loaded" line */
static int page_code;
static int page_cut;
static int ld_img_idx = -1;
static int ld_script_idx = -1;
static int ld_sheet_idx = -1;
static int no_more_needs = 0;          /* set by Stop; poll_husser_needs stays quiet */
static int resources_busy = 0;         /* a stylesheet/image fetch is in flight or pending */
static void loaded_status(void);

/* ---- ultra mode: a memory cache of subresources (scripts, stylesheets, images) ----
 * On a machine with lots of RAM, what was fetched once is kept, so going back to a site
 * (YouTube's scripts are 15 MB) skips the network entirely. */
#define CACHE_MAX_BYTES (48u * 1024 * 1024)
#define CACHE_SLOTS 64
static struct { char url[URL_MAX]; u8 *data; u32 len; u64 used; } rcache[CACHE_SLOTS];
static u32 rcache_bytes = 0;
static int js_done = 0, cache_hits = 0;
static int ultra_mode(void){
    const volatile husser_shm_t *h = husser_header();
    return h && h->ultra;
}
static int cache_find(const char *url){
    for(int i = 0; i < CACHE_SLOTS; i++) if(rcache[i].data && same(rcache[i].url, url)){ rcache[i].used = timer_ticks(); return i; }
    return -1;
}
static void cache_put(const char *url, const void *data, u32 len){
    if(!ultra_mode() || len == 0 || len > CACHE_MAX_BYTES / 2 || cache_find(url) >= 0) return;
    /* make room: drop the least recently used */
    while(rcache_bytes + len > CACHE_MAX_BYTES){
        int old = -1;
        for(int i = 0; i < CACHE_SLOTS; i++) if(rcache[i].data && (old < 0 || rcache[i].used < rcache[old].used)) old = i;
        if(old < 0) break;
        kfree(rcache[old].data); rcache_bytes -= rcache[old].len; rcache[old].data = NULL;
    }
    int slot = -1;
    for(int i = 0; i < CACHE_SLOTS && slot < 0; i++) if(!rcache[i].data) slot = i;
    if(slot < 0) return;
    u8 *copy = kmalloc(len);
    if(!copy) return;
    memcpy(copy, data, len);
    ui_copy(rcache[slot].url, url, URL_MAX);
    rcache[slot].data = copy; rcache[slot].len = len; rcache[slot].used = timer_ticks();
    rcache_bytes += len;
}

static void ld_cleanup(void){
    if(ld_sock >= 0) tcp_close(ld_sock);
    ld_sock = -1;
    if(ld_buf) kfree(ld_buf);
    ld_buf = NULL;
    ld = LD_IDLE;
}

static void push_history(void){
    if(!cur_url[0]) return;
    if(nhist < HIST_MAX){ ui_copy(hist[nhist], cur_url, URL_MAX); hist_scroll[nhist] = scroll; nhist++; return; }
    for(int i = 1; i < HIST_MAX; i++){ ui_copy(hist[i - 1], hist[i], URL_MAX); hist_scroll[i - 1] = hist_scroll[i]; }
    ui_copy(hist[HIST_MAX - 1], cur_url, URL_MAX);
    hist_scroll[HIST_MAX - 1] = scroll;
}

/* Shows html as the page for url: spawns or reuses Husser, hands it the page. */
static rect_t area_of(rect_t b);
static int show(const char *url, const char *html, u32 len){
    scroll = ld_scroll >= 0 ? ld_scroll : 0;
    rect_t area = area_of(last_body);
    if(husser_open(area.w, area.h) < 0) return -1;
    { volatile husser_shm_t *hs = (volatile husser_shm_t *)husser_header(); if(hs) hs->adblock = (u32)browser_adblock; }
    husser_set_url(url);
    husser_load(html, len, (u32)scroll);
    for(int j = 0; j < RES_KINDS; j++) for(int k = 0; k < RES_IDX; k++) res_done[j][k] = 0;
    need_seq_consumed = 0;
    no_more_needs = 0;
    resources_busy = 0;
    waiting_link = -1;
    if(ld_remember && !same(cur_url, url)) push_history();
    ui_copy(cur_url, url, URL_MAX);
    if(!editing){ ui_copy(addr, url, sizeof addr); addr_len = (int)ui_len(addr); }
    have_page = 1;
    return 0;
}

static void fail(const char *why){
    static char page[2048];
    ld_cleanup();
    if(ld_kind >= 1 && ld_kind <= 3){   /* a stylesheet/image/script that fails is just left out */
        int idx = ld_kind == 1 ? ld_sheet_idx : ld_kind == 2 ? ld_img_idx : ld_script_idx;
        husser_res((u32)ld_kind, (u32)(idx < 0 ? 0 : idx), NULL, RES_FAIL_LEN);
        ld_kind = 0;
        ld_cleanup();
        return;
    }
    ui_copy(page, "<!DOCTYPE html><html><head><title>Can't open this page</title><style>"
                  "html{background:#1c2328}body{margin:24px}h1{color:#ffb547}p{color:#9aa7b0}a{color:#4cc2ff}"
                  "</style></head><body><h1>Can't open this page</h1><p>", sizeof page);
    esc_cat(page, why, sizeof page);
    cat_n(page, "</p><p>Address: ", sizeof page);
    esc_cat(page, ld_url, sizeof page);
    cat_n(page, "</p><p><a href=\"", sizeof page);
    esc_cat(page, ld_url, sizeof page);
    cat_n(page, "\">Try again</a></p></body></html>", sizeof page);
    show(ld_url, page, ui_len(page));
    ui_copy(status, why, sizeof status);
    tell("Couldn't open the page", why);
}

/* A search engine's click redirector (DuckDuckGo's /l/?uddg=<address>): the address itself,
 * one hop and one tracking request fewer. */
static void skip_redirector(char *url, u32 max){
    if(!starts(url, "https://duckduckgo.com/l/?") && !starts(url, "http://duckduckgo.com/l/?") &&
       !starts(url, "https://html.duckduckgo.com/l/?")) return;
    const char *p = url;
    while(*p && !(starts(p, "uddg=") && (p[-1] == '?' || p[-1] == '&'))) p++;
    if(!*p) return;
    p += 5;
    char out[URL_MAX]; u32 k = 0;
    for(; *p && *p != '&' && k + 1 < sizeof out; p++){
        int hi, lo;
        if(*p == '%' && p[1] && p[2]){
            hi = p[1] <= '9' ? p[1] - '0' : (p[1] | 32) - 'a' + 10;
            lo = p[2] <= '9' ? p[2] - '0' : (p[2] | 32) - 'a' + 10;
            if(hi >= 0 && hi < 16 && lo >= 0 && lo < 16){ out[k++] = (char)(hi * 16 + lo); p += 2; continue; }
        }
        out[k++] = *p == '+' ? ' ' : *p;
    }
    out[k] = 0;
    if(starts(out, "http://") || starts(out, "https://")) ui_copy(url, out, max);
}

/* Starts loading url. The page on screen stays until the new one arrives. */
static void fetch_cancel_all(void);
static int fetch_active(void);
static void download(const u8 *body, u32 blen, const char *ctype, const char *disp);
static int ad_url(const char *url);
/* a page's own fetch() calls (a video streaming) do not count as loading the page */
static int page_fetches(void);
static int loading(void){ return ld != LD_IDLE || page_fetches(); }
static void media_close(void);
static void go(const char *url, int remember, int scroll_to){
    ld_cleanup();
    fetch_cancel_all();
    media_close();                       /* a new page: the old page's video stops */
    ld_kind = 0;
    css_done = 0;
    img_done = 0;
    js_done = 0; cache_hits = 0;
    no_more_needs = 0;
    for(int j = 0; j < RES_KINDS; j++) for(int k = 0; k < RES_IDX; k++) res_done[j][k] = 0;
    need_seq_consumed = 0;
    waiting_link = -1;
    char full[URL_MAX];
    typed_url(url, full, sizeof full);
    skip_redirector(full, sizeof full);
    if(browser_adblock && ad_url(full)){
        ui_copy(status, "Blocked a page from an ad server.", sizeof status);
        return;
    }
    ads_blocked = 0;
    ui_copy(ld_url, full, URL_MAX);
    ld_remember = remember;
    ld_scroll = scroll_to;
    ld_redirects = 0;
    ld_t0 = timer_ticks();
    editing = 0;
    ui_copy(addr, full, sizeof addr);
    addr_len = (int)ui_len(addr);
    if(same(full, "about:start") || same(full, "about:blank")){
        show(full, start_page, sizeof start_page - 1);
        ui_copy(status, "", sizeof status);
        return;
    }
    url_t u;
    if(parse_url(full, &u) != 0){ fail("That is not a web address. Try something like example.com."); return; }
    ui_copy(ld_host, u.host, sizeof ld_host);
    ld = LD_START;
    ui_copy(status, "Connecting to ", sizeof status);
    cat_n(status, u.host, sizeof status);
    cat_n(status, "...", sizeof status);
}

static void stop(void){
    if(!loading()) return;
    if(ld_kind >= 1 && ld_kind <= 3){   /* stop a subresource: the page itself is here */
        int idx = ld_kind == 1 ? ld_sheet_idx : ld_kind == 2 ? ld_img_idx : ld_script_idx;
        husser_res((u32)ld_kind, (u32)(idx < 0 ? 0 : idx), NULL, RES_FAIL_LEN);
        ld_kind = 0;
    }
    no_more_needs = 1;
    ld_cleanup();
    fetch_cancel_all();
    ui_copy(addr, cur_url, sizeof addr);
    addr_len = (int)ui_len(addr);
    ui_copy(status, "Stopped.", sizeof status);
}

/* Opens the connection and sends the request. Blocks for DNS and the TCP handshake only. */
static void ld_connect(void){
    url_t u;
    if(parse_url(ld_url, &u) != 0){ fail("That is not a web address."); return; }
    if(!net_up()){ fail("There is no network. Check the Network app."); return; }

    u32 ip = 0;
    u16 port = u.port;
    char host[HOST_MAX], path[PATH_MAX_B + HOST_MAX + 16];
    ui_copy(host, u.host, sizeof host);
    ui_copy(path, u.path, sizeof path);
    if(u.https){
        /* ask the host's proxy: GET /https/<host>[:port]/<path> */
        ui_copy(path, "/https/", sizeof path);
        cat_n(path, u.host, sizeof path);
        if(u.port != 443){ char n[8] = ""; ui_num(u.port, n); cat_n(path, ":", sizeof path); cat_n(path, n, sizeof path); }
        cat_n(path, u.path, sizeof path);
        ui_copy(host, "10.0.2.2", sizeof host);
        ip = PROXY_IP;
        port = PROXY_PORT;
    } else if(net_resolve(u.host, &ip, 0, 0) != 0){
        char m[128] = "Could not find the server ";
        cat_n(m, u.host, sizeof m);
        cat_n(m, " (DNS lookup failed).", sizeof m);
        fail(m);
        return;
    }

    ld_sock = tcp_connect(ip, port);
    if(ld_sock < 0){
        fail(u.https ? "Could not reach the HTTPS proxy on the host. Start it with: python3 tools/https_proxy.py"
                     : "The server did not accept the connection.");
        return;
    }
    char req[PATH_MAX_B + HOST_MAX + 256] = "GET ";
    cat_n(req, path, sizeof req);
    cat_n(req, " HTTP/1.0\r\nHost: ", sizeof req);
    cat_n(req, host, sizeof req);
    cat_n(req, ld_kind == 1 ? "\r\nUser-Agent: BRO-weser\r\nAccept: text/css,*/*\r\nConnection: close\r\n\r\n"
             : ld_kind == 2 ? "\r\nUser-Agent: BRO-weser\r\nAccept: image/*,*/*;q=0.8\r\nConnection: close\r\n\r\n"
             : ld_kind == 3 ? "\r\nUser-Agent: BRO-weser\r\nAccept: */*\r\nConnection: close\r\n\r\n"
                            : "\r\nUser-Agent: BRO-weser\r\nAccept: text/html,text/plain\r\nConnection: close\r\n\r\n", sizeof req);
    if(tcp_send(ld_sock, req, ui_len(req)) != 0){ fail("Sending the request failed."); return; }

    ld_cap = 256 * 1024;                     /* grows as the answer comes (a download can be big) */
    ld_buf = kmalloc(ld_cap + 1);
    if(!ld_buf){ fail("Out of memory."); return; }
    ld_len = ld_hdr = ld_clen = 0;
    ld_have_clen = 0;
    ld_tlast = timer_ticks();
    ld = LD_RECV;
    if(ld_kind >= 1 && ld_kind <= 3) return;   /* the caller already set a status line */
    ui_copy(status, "Waiting for ", sizeof status);
    cat_n(status, ld_host, sizeof status);
    cat_n(status, "...", sizeof status);
}

/* The value of header name (lower case, with the colon), copied into out. 0 if missing. */
static int header(const char *name, char *out, u32 max){
    u32 nl = ui_len(name);
    for(u32 i = 0; i + nl < ld_hdr; i++){
        if(i > 0 && ld_buf[i - 1] != '\n') continue;
        u32 k = 0;
        while(k < nl && lower(ld_buf[i + k]) == name[k]) k++;
        if(k < nl) continue;
        u32 p = i + nl, n = 0;
        while(p < ld_hdr && ld_buf[p] == ' ') p++;
        while(p < ld_hdr && ld_buf[p] != '\r' && ld_buf[p] != '\n' && n + 1 < max) out[n++] = (char)ld_buf[p++];
        out[n] = 0;
        return 1;
    }
    return 0;
}

static void ld_finish(void){
    tcp_close(ld_sock);
    ld_sock = -1;
    if(ld_hdr == 0 || ld_len < 12 || !starts((const char *)ld_buf, "HTTP/")){
        fail(ld_len ? "The server's answer was not HTTP." : "The server closed the connection without answering.");
        return;
    }
    int code = (ld_buf[9] - '0') * 100 + (ld_buf[10] - '0') * 10 + (ld_buf[11] - '0');
    char val[URL_MAX];

    if(code == 502 && header("x-proxy-error:", val, sizeof val)){
        char m[200] = "The HTTPS proxy could not fetch the page: ";
        cat_n(m, val, sizeof m);
        fail(m);
        return;
    }
    if(code >= 300 && code < 400 && header("location:", val, sizeof val)){
        if(++ld_redirects > MAX_REDIRECTS){ fail("The site redirected too many times."); return; }
        char next[URL_MAX];
        resolve(ld_url, val, next, sizeof next);
        ui_copy(ld_url, next, URL_MAX);
        kfree(ld_buf); ld_buf = NULL;
        url_t u;
        if(parse_url(next, &u) != 0){ fail("The site redirected to an address Browser cannot open."); return; }
        ui_copy(ld_host, u.host, sizeof ld_host);
        ld = LD_START;
        ui_copy(status, "Redirected to ", sizeof status);
        cat_n(status, u.host, sizeof status);
        return;
    }
    /* the page's real address after the proxy followed redirects */
    if(header("x-final-url:", val, sizeof val) && starts(val, "https://")) ui_copy(ld_url, val, URL_MAX);

    u32 blen = ld_len - ld_hdr;
    const char *body = (const char *)ld_buf + ld_hdr;
    if(ld_kind == 2){
        /* an image: husser decodes it */
        if(ld_img_idx >= 0 && code < 300) husser_res(2, (u32)ld_img_idx, body, blen);
        else husser_res(2, (u32)(ld_img_idx < 0 ? 0 : ld_img_idx), NULL, RES_FAIL_LEN);
        img_done++;
        ld_kind = 0;
        ld_cleanup();
        return;
    }
    if(ld_kind >= 1 && ld_kind <= 3 && code < 300 && blen) cache_put(ld_url, body, blen);
    if(ld_kind == 3){
        /* a script: husser runs the page's scripts once all of them are in */
        if(code < 300){ husser_res(3, (u32)(ld_script_idx < 0 ? 0 : ld_script_idx), body, blen); js_done++; }
        else husser_res(3, (u32)(ld_script_idx < 0 ? 0 : ld_script_idx), NULL, RES_FAIL_LEN);
        ld_kind = 0;
        ld_cleanup();
        return;
    }
    if(ld_kind == 1){
        /* a stylesheet: husser applies it and lays out again */
        char ct[80] = "text/css";
        header("content-type:", ct, sizeof ct);
        int not_css = 0;
        for(char *q = ct; *q; q++) if(starts(q, "javascript") || starts(q, "html")) not_css = 1;
        if(code < 300 && blen && !not_css){ husser_res(1, (u32)(ld_sheet_idx < 0 ? 0 : ld_sheet_idx), body, blen); css_done++; }
        else husser_res(1, (u32)(ld_sheet_idx < 0 ? 0 : ld_sheet_idx), NULL, RES_FAIL_LEN);
        ld_kind = 0;
        ld_cleanup();
        return;
    }
    char ctype[80] = "text/html";
    header("content-type:", ctype, sizeof ctype);
    for(char *q = ctype; *q; q++) *q = (char)lower(*q);

    if(code >= 400 && blen == 0){
        char m[96] = "The server answered with error ";
        char n[8] = ""; ui_num((u32)code, n);
        cat_n(m, n, sizeof m);
        cat_n(m, ".", sizeof m);
        fail(m);
        return;
    }
    int html = 0, text = 0, markdown = 0;
    for(char *q = ctype; *q; q++) if(starts(q, "html") || starts(q, "xml")) html = 1;
    for(char *q = ctype; *q; q++) if(starts(q, "markdown")) markdown = 1;
    /* a .md file (servers send it as text/plain or octet-stream): shown formatted */
    if(!html && (markdown || md_is_markdown_path(ld_url))) markdown = 1, html = 0, text = 0;
    else if(!html && starts(ctype, "text/")) text = 1;
    char disp[160] = "";
    int attachment = header("content-disposition:", disp, sizeof disp) && (disp[0] == 'a' || disp[0] == 'A');
    if((!html && !text && !markdown) || attachment){
        /* not a page: a download, saved to the disk */
        if(ld_len >= LOAD_MAX){ fail("The file is too big to download here."); return; }
        download(body, blen, ctype, disp);
        return;
    }

    int r;
    if(markdown){
        /* the page's name: the file's, from the end of the address */
        char title[96] = "";
        const char *slash = ld_url;
        for(const char *q = ld_url; *q && *q != '?'; q++) if(*q == '/') slash = q + 1;
        u32 k = 0;
        while(slash[k] && slash[k] != '?' && k + 1 < sizeof title){ title[k] = slash[k]; k++; }
        title[k] = 0;
        u32 plen = 0;
        char *page = md_page(body, blen, title, &plen);
        if(!page){ fail("Out of memory."); return; }
        r = show(ld_url, page, plen);
        kfree(page);
    } else if(text){
        /* plain text: escaped, with a line break per line */
        u32 cap = blen * 5 + 64;
        char *page = kmalloc(cap);
        if(!page){ fail("Out of memory."); return; }
        u32 n = 0;
        const char *head = "<html><body><p>";
        for(const char *s = head; *s; s++) page[n++] = *s;
        for(u32 i = 0; i < blen && n + 8 < cap; i++){
            char c = body[i];
            const char *rep = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '\n' ? "<br>" : 0;
            if(rep) while(*rep) page[n++] = *rep++;
            else if(c != '\r') page[n++] = c;
        }
        const char *tail = "</p></body></html>";
        for(const char *s = tail; *s && n + 1 < cap; s++) page[n++] = *s;
        r = show(ld_url, page, n);
        kfree(page);
    } else r = show(ld_url, body, blen);
    if(r != 0){ fail("The page could not be read."); return; }

    page_kb = (blen + 1023) / 1024;
    page_code = code;
    page_cut = ld_len >= LOAD_MAX;
    ld_cleanup();
    /* the page is shown; Husser now asks for the stylesheets and images it wants */
    loaded_status();
}

/* A line on the serial port (for timing pages from the host). */
static void serial_log(const char *what){
    for(const char *q = "browser: "; *q; q++) serial_putc(*q);
    for(const char *q = what; *q; q++) serial_putc(*q);
    serial_putc(' '); serial_putc('|'); serial_putc(' ');
    for(const char *q = cur_url; *q; q++) serial_putc(*q);
    serial_putc('\n');
}

/* "Loaded 14 KB + 2 stylesheets in 0.8 s" (or the server's error code) */
static void loaded_status(void){
    u32 tenths = (u32)((timer_ticks() - ld_t0) / 10);
    char m[128] = "";
    if(page_code >= 400){
        char n[8] = ""; ui_num((u32)page_code, n);
        ui_copy(m, "Server error ", sizeof m);
        cat_n(m, n, sizeof m);
        cat_n(m, " - ", sizeof m);
    }
    char kb[12] = "", sec[12] = "", ten[4] = "", ns[8] = "";
    ui_num(page_kb, kb);
    ui_num(tenths / 10, sec);
    ui_num(tenths % 10, ten);
    cat_n(m, "Loaded ", sizeof m); cat_n(m, kb, sizeof m); cat_n(m, " KB", sizeof m);
    if(css_done){
        ui_num((u32)css_done, ns);
        cat_n(m, " + ", sizeof m); cat_n(m, ns, sizeof m);
        cat_n(m, css_done == 1 ? " stylesheet" : " stylesheets", sizeof m);
    }
    if(img_done){
        ui_num((u32)img_done, ns);
        cat_n(m, " + ", sizeof m); cat_n(m, ns, sizeof m);
        cat_n(m, img_done == 1 ? " image" : " images", sizeof m);
    }
    if(js_done){
        ui_num((u32)js_done, ns);
        cat_n(m, " + ", sizeof m); cat_n(m, ns, sizeof m);
        cat_n(m, js_done == 1 ? " script" : " scripts", sizeof m);
    }
    cat_n(m, " in ", sizeof m);
    cat_n(m, sec, sizeof m); cat_n(m, ".", sizeof m); cat_n(m, ten, sizeof m); cat_n(m, " s", sizeof m);
    if(ads_blocked){ ui_num((u32)ads_blocked, ns); cat_n(m, " | ", sizeof m); cat_n(m, ns, sizeof m); cat_n(m, ads_blocked == 1 ? " ad blocked" : " ads blocked", sizeof m); }
    if(page_cut) cat_n(m, " (cut off)", sizeof m);
    serial_log(m);
    if(ultra_mode()){
        cat_n(m, " | Ultra mode", sizeof m);
        if(cache_hits){ ui_num((u32)cache_hits, ns); cat_n(m, ", ", sizeof m); cat_n(m, ns, sizeof m); cat_n(m, " from memory", sizeof m); }
    }
    ui_copy(status, m, sizeof status);
}

/* Reads whatever has arrived, without waiting. */
static void ld_recv(void){
    /* read for at most a tick (10 ms) at a time: fast, and the desktop stays smooth */
    u64 now = timer_ticks(), until = now + (ultra_mode() ? 4 : 1);   /* ultra: bigger bites */
    for(;;){
        net_poll();
        if(ld_len == ld_cap){
            u32 max = LOAD_MAX;
            if(ld_cap >= max) break;
            u32 nc = ld_cap * 2 > max ? max : ld_cap * 2;
            if(ld_have_clen && ld_hdr + ld_clen > nc && ld_hdr + ld_clen <= max) nc = ld_hdr + ld_clen;
            u8 *g = kmalloc(nc + 1);
            if(!g) break;
            memcpy(g, ld_buf, ld_len);
            kfree(ld_buf);
            ld_buf = g; ld_cap = nc;
        }
        u32 room = ld_cap - ld_len;
        int got = tcp_recv(ld_sock, ld_buf + ld_len, room < 65536 ? room : 65536, 0);
        if(got > 0){ ld_len += (u32)got; ld_tlast = timer_ticks(); continue; }
        if(tcp_closed(ld_sock) || timer_ticks() >= until) break;
    }
    now = timer_ticks();
    if(ld_hdr == 0){
        for(u32 i = 0; i + 3 < ld_len; i++)
            if(ld_buf[i] == '\r' && ld_buf[i + 1] == '\n' && ld_buf[i + 2] == '\r' && ld_buf[i + 3] == '\n'){
                ld_hdr = i + 4;
                char v[16];
                if(header("content-length:", v, sizeof v)){
                    ld_clen = 0;
                    for(char *q = v; *q >= '0' && *q <= '9'; q++) ld_clen = ld_clen * 10 + (u32)(*q - '0');
                    ld_have_clen = 1;
                }
                break;
            }
    }
    if(ld_len > 0 && ld_kind == 0){
        char kb[12] = "";
        ui_num(ld_len / 1024, kb);
        ui_copy(status, "Receiving ", sizeof status);
        cat_n(status, kb, sizeof status);
        cat_n(status, " KB from ", sizeof status);
        cat_n(status, ld_host, sizeof status);
        cat_n(status, "...", sizeof status);
    }
    int done = tcp_closed(ld_sock) || ld_len >= LOAD_MAX ||
               (ld_have_clen && ld_hdr && ld_len >= ld_hdr + ld_clen);
    if(done){ ld_finish(); return; }
    if(ld_len == 0 && now - ld_tlast > FIRST_BYTE_TICKS){ fail("The server did not answer in time."); return; }
    if(ld_len > 0 && now - ld_tlast > IDLE_TICKS){
        if(ld_hdr) ld_finish();                 /* show what came */
        else fail("The server stopped sending.");
    }
}



/* ---- downloads: files that are not pages are saved under HOME/DOWNLOAD ---- */

static int ends_ci(const char *s, const char *e){
    u32 n = ui_len(s), m = ui_len(e);
    if(m > n) return 0;
    for(u32 i = 0; i < m; i++) if(lower(s[n - m + i]) != e[i]) return 0;
    return 1;
}
/* The folder a file goes in, by its type or extension. */
static const char *download_folder(const char *ctype, const char *ext){
    static const struct { const char *folder, *types, *exts; } kinds[] = {
        { "PICTURES", "image/", " jpg jpeg png gif bmp webp svg ico tif tiff avif " },
        { "VIDEOS",   "video/", " avi mp4 mkv webm mov m4v mpg mpeg wmv flv " },
        { "MUSIC",    "audio/", " mp3 wav ogg flac m4a aac opus wma " },
        { "DOCS",     "application/pdf", " pdf doc docx odt txt md rtf xls xlsx ods ppt pptx odp csv epub " },
        { "ARCHIVES", "application/zip", " zip tar gz tgz bz2 xz 7z rar iso adp img " },
    };
    char e[12] = " ";
    u32 k = 1;
    for(const char *q = ext; *q && k < 9; q++) e[k++] = (char)lower(*q);
    e[k++] = ' '; e[k] = 0;
    for(u32 i = 0; i < sizeof kinds / sizeof kinds[0]; i++){
        if(starts(ctype, kinds[i].types)) return kinds[i].folder;
        if(k > 2){
            for(const char *q = kinds[i].exts; *q; q++){
                const char *a = q, *b = e;
                while(*b && *a == *b){ a++; b++; }
                if(!*b) return kinds[i].folder;
            }
        }
    }
    return "OTHER";
}
/* An 8.3 name (the disk's FAT32 has no long names) from a file name: "My Photo.jpeg" -> MYPHOTO.JPE */
static void short_name(const char *name, char *out){
    const char *dot = NULL;
    for(const char *q = name; *q; q++) if(*q == '.') dot = q;
    u32 k = 0;
    for(const char *q = name; *q && q != dot && k < 8; q++){
        char c = *q;
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out[k++] = c;
    }
    if(k == 0){ const char *d = "DOWNLOAD"; while(*d) out[k++] = *d++; }
    if(dot && dot[1]){
        out[k++] = '.';
        u32 e = 0;
        for(const char *q = dot + 1; *q && e < 3; q++){
            char c = *q;
            if(c >= 'a' && c <= 'z') c = (char)(c - 32);
            if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')){ out[k++] = c; e++; }
        }
        if(out[k - 1] == '.') k--;
    }
    out[k] = 0;
}
static int file_exists(const char *path){
    fat32_file_t f;
    if(fat32_open(path, &f) != 0) return 0;
    fat32_close(&f);
    return 1;
}

static void download(const u8 *body, u32 blen, const char *ctype, const char *disp){
    /* the file's name: Content-Disposition's filename=, or the end of the address */
    char name[128] = "";
    const char *fn = disp;
    while(*fn && !starts(fn, "filename=")) fn++;
    if(*fn){
        fn += 9;
        if(*fn == '"') fn++;
        u32 k = 0;
        while(*fn && *fn != '"' && *fn != ';' && k + 1 < sizeof name) name[k++] = *fn++;
        name[k] = 0;
    }
    if(!name[0]){
        const char *slash = ld_url;
        for(const char *q = ld_url; *q && *q != '?' && *q != '#'; q++) if(*q == '/') slash = q + 1;
        u32 k = 0;
        for(const char *q = slash; *q && *q != '?' && *q != '#' && k + 1 < sizeof name; q++){
            if(*q == '%' && q[1] && q[2]){                  /* %20 and the like */
                int hi = q[1] <= '9' ? q[1] - '0' : (q[1] | 32) - 'a' + 10, lo = q[2] <= '9' ? q[2] - '0' : (q[2] | 32) - 'a' + 10;
                if(hi >= 0 && hi < 16 && lo >= 0 && lo < 16){ name[k++] = (char)(hi * 16 + lo); q += 2; continue; }
            }
            name[k++] = *q;
        }
        name[k] = 0;
    }
    /* no extension: one from the type */
    int has_dot = 0;
    for(const char *q = name; *q; q++) if(*q == '.') has_dot = 1;
    if(!has_dot){
        static const struct { const char *t, *e; } te[] = {
            { "image/jpeg", ".jpg" }, { "image/png", ".png" }, { "image/gif", ".gif" }, { "image/webp", ".webp" },
            { "image/svg", ".svg" }, { "video/mp4", ".mp4" }, { "video/x-msvideo", ".avi" }, { "video/avi", ".avi" },
            { "video/webm", ".webm" }, { "audio/mpeg", ".mp3" }, { "audio/wav", ".wav" }, { "application/pdf", ".pdf" },
            { "application/zip", ".zip" }, { "application/json", ".json" },
        };
        for(u32 i = 0; i < sizeof te / sizeof te[0]; i++) if(starts(ctype, te[i].t)){ cat_n(name, te[i].e, sizeof name); break; }
    }
    const char *ext = "";
    for(const char *q = name; *q; q++) if(*q == '.') ext = q + 1;
    char sn[16];
    short_name(name, sn);
    /* the folder, made if it is not there */
    char dir[48] = "HOME/DOWNLOAD";
    fat32_mkdir("HOME");
    fat32_mkdir("HOME/DOWNLOAD");
    if(browser_sort_downloads){
        cat_n(dir, "/", sizeof dir);
        cat_n(dir, download_folder(ctype, ext), sizeof dir);
        fat32_mkdir(dir);
    }
    /* a free name: NAME.EXT, then NAME~1.EXT ... */
    char path[96];
    ui_copy(path, dir, sizeof path); cat_n(path, "/", sizeof path); cat_n(path, sn, sizeof path);
    for(int n = 1; n < 100 && file_exists(path); n++){
        char base[16], ex[8] = "", num[4] = "";
        u32 k = 0; const char *q = sn;
        while(*q && *q != '.' && k < 15) base[k++] = *q++;
        base[k] = 0;
        if(*q == '.') ui_copy(ex, q, sizeof ex);
        u32 room = n < 10 ? 6 : 5;
        if(ui_len(base) > room) base[room] = 0;
        ui_num((u32)n, num);
        ui_copy(path, dir, sizeof path); cat_n(path, "/", sizeof path);
        cat_n(path, base, sizeof path); cat_n(path, "~", sizeof path); cat_n(path, num, sizeof path); cat_n(path, ex, sizeof path);
    }
    int ok = fat32_write_path(path, body, blen) == 0;
    static char page[2048];
    char kb[12] = ""; ui_num((blen + 1023) / 1024, kb);
    ui_copy(page, "<!DOCTYPE html><html><head><title>Download</title><style>"
                  "html{background:#1c2328}body{margin:24px;color:#c9d4dc}h1{color:#3ddc97}p{color:#9aa7b0}b{color:#f2f5f7}a{color:#4cc2ff}"
                  ".bad{color:#ffb547}</style></head><body>", sizeof page);
    if(ok){
        cat_n(page, "<h1>Downloaded</h1><p><b>", sizeof page); esc_cat(page, name[0] ? name : sn, sizeof page);
        cat_n(page, "</b> (", sizeof page); cat_n(page, kb, sizeof page);
        cat_n(page, " KB) was saved as</p><p><b>", sizeof page); esc_cat(page, path, sizeof page);
        cat_n(page, "</b></p><p>Open it from the Files app.", sizeof page);
        if(browser_sort_downloads) cat_n(page, " Downloads are sorted into folders by kind (Settings can turn that off).", sizeof page);
        cat_n(page, "</p>", sizeof page);
    } else {
        cat_n(page, "<h1 class=\"bad\">Could not save the download</h1><p>Writing <b>", sizeof page);
        esc_cat(page, path, sizeof page);
        cat_n(page, "</b> failed: is a disk attached, and is there room on it?</p>", sizeof page);
    }
    cat_n(page, "<p>From: ", sizeof page); esc_cat(page, ld_url, sizeof page); cat_n(page, "</p></body></html>", sizeof page);
    char from[URL_MAX]; ui_copy(from, ld_url, sizeof from);
    show(from, page, ui_len(page));
    ld_cleanup();
    char m[128];
    ui_copy(m, ok ? "Downloaded to " : "Download failed: ", sizeof m); cat_n(m, path, sizeof m);
    ui_copy(status, m, sizeof status);
    tell(ok ? "Download finished" : "Download failed", path);
    serial_log(m);
}

/* ---- the ad blocker: requests to ad and tracking servers are never made ---- */
static const char *const ad_hosts[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "google-analytics.com", "googletagmanager.com",
    "googletagservices.com", "adservice.google.com", "pagead2.googlesyndication.com", "amazon-adsystem.com", "adnxs.com",
    "taboola.com", "outbrain.com", "criteo.com", "criteo.net", "scorecardresearch.com", "quantserve.com", "quantcount.com",
    "pubmatic.com", "rubiconproject.com", "openx.net", "adsrvr.org", "moatads.com", "2mdn.net", "adform.net",
    "media.net", "yieldmo.com", "teads.tv", "sharethrough.com", "smartadserver.com", "casalemedia.com", "indexww.com",
    "bidswitch.net", "mathtag.com", "everesttech.net", "chartbeat.com", "chartbeat.net", "hotjar.com", "ads-twitter.com",
    "analytics.twitter.com", "connect.facebook.net", "analytics.tiktok.com", "static.cloudflareinsights.com",
    "adcolony.com", "applovin.com", "unityads.unity3d.com", "zedo.com", "popads.net", "popcash.net", "propellerads.com",
    "adsterra.com", "exoclick.com", "trafficjunky.net", "juicyads.com", "mgid.com", "revcontent.com", "adroll.com",
    "segment.io", "mixpanel.com", "optimizely.com", "newrelic.com", "nr-data.net", "bat.bing.com", "clarity.ms",
    "imasdk.googleapis.com", "securepubads.g.doubleclick.net", "adsafeprotected.com", "doubleverify.com", "33across.com",
    "lijit.com", "sovrn.com", "contextweb.com", "gumgum.com", "triplelift.com", "yahoo.com/ads", "advertising.com",
    0 };
static int ad_url(const char *url){
    url_t u;
    if(parse_url(url, &u) != 0) return 0;
    u32 hl = ui_len(u.host);
    for(int i = 0; ad_hosts[i]; i++){
        u32 al = ui_len(ad_hosts[i]);
        if(al > hl) continue;
        /* the host itself or a subdomain of it */
        if(same(u.host + hl - al, ad_hosts[i]) && (al == hl || u.host[hl - al - 1] == '.')) return 1;
    }
    /* well-known ad paths on any server */
    static const char *const paths[] = { "/pagead/", "/adsbygoogle", "/ads.js", "/adserver", "/doubleclick/", "/prebid", "/gpt.js", 0 };
    for(int i = 0; paths[i]; i++) for(const char *q = u.path; *q; q++) if(starts(q, paths[i])) return 1;
    return 0;
}

/* ---- subresources: stylesheets, scripts and images, FETCH_SLOTS at a time ----
 * Husser lists what the page needs; each need gets a slot with its own connection, and the
 * answers queue up until Husser is free to take one. The network never waits for Husser,
 * and Husser never waits for one slow server. */
#define FETCH_SLOTS 6
#define PEND_MAX    256
enum { F_FREE, F_START, F_RECV, F_DONE };
typedef struct {
    int state;
    u32 kind, idx;
    char url[URL_MAX];
    int sock;
    u8 *buf; u32 len, cap, hdr, clen; int have_clen;
    u64 tlast;
    int redirects;
    int ok;                       /* F_DONE: 1 with a body, 0 failed */
    u32 status;                   /* the HTTP status (script fetches see it) */
    const u8 *body; u32 blen;     /* F_DONE: what goes to Husser */
    int cached;                   /* the body is the ultra-mode cache's (not ours to free) */
} fetch_t;
static fetch_t fslot[FETCH_SLOTS];
static struct { u32 kind, idx; char url[URL_MAX]; } pend[PEND_MAX];   /* kind 4: a script's fetch(), idx its id */
static int npend = 0;
static int fetch_total = 0;       /* started for this page, for the status line */

static void fetch_free(fetch_t *f){
    if(f->sock >= 0) tcp_close(f->sock);
    f->sock = -1;
    if(f->buf) kfree(f->buf);
    f->buf = NULL;
    f->len = f->cap = f->hdr = f->clen = 0;
    f->have_clen = 0;
    f->state = F_FREE;
}
static void fetch_cancel_all(void){
    for(int i = 0; i < FETCH_SLOTS; i++){ if(fslot[i].state == F_FREE) fslot[i].sock = -1; fetch_free(&fslot[i]); }
    npend = 0;
    fetch_total = 0;
}
static int fetch_active(void){
    int n = 0;
    for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state != F_FREE) n++;
    return n + npend;
}
static int page_fetches(void){
    int n = 0;
    for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state != F_FREE && fslot[i].kind != 4) n++;
    for(int k = 0; k < npend; k++) if(pend[k].kind != 4) n++;
    return n;
}
static void fetch_done(fetch_t *f, int ok, const u8 *body, u32 blen){
    if(f->sock >= 0) tcp_close(f->sock);
    f->sock = -1;
    f->state = F_DONE;
    f->ok = ok && body && blen;
    f->body = body; f->blen = blen;
}

/* Opens the connection and sends the request (DNS and the handshake block briefly). */
static void fetch_connect(fetch_t *f){
    url_t u;
    if(parse_url(f->url, &u) != 0 || !net_up()){ fetch_done(f, 0, NULL, 0); return; }
    u32 ip = 0; u16 port = u.port;
    char host[HOST_MAX], path[PATH_MAX_B + HOST_MAX + 16];
    ui_copy(host, u.host, sizeof host);
    ui_copy(path, u.path, sizeof path);
    if(u.https){
        ui_copy(path, "/https/", sizeof path);
        cat_n(path, u.host, sizeof path);
        if(u.port != 443){ char n[8] = ""; ui_num(u.port, n); cat_n(path, ":", sizeof path); cat_n(path, n, sizeof path); }
        cat_n(path, u.path, sizeof path);
        ui_copy(host, "10.0.2.2", sizeof host);
        ip = PROXY_IP; port = PROXY_PORT;
    } else if(net_resolve(u.host, &ip, 0, 0) != 0){ fetch_done(f, 0, NULL, 0); return; }
    f->sock = tcp_connect(ip, port);
    if(f->sock < 0){ fetch_done(f, 0, NULL, 0); return; }
    char req[PATH_MAX_B + HOST_MAX + 256] = "GET ";
    cat_n(req, path, sizeof req);
    cat_n(req, " HTTP/1.0\r\nHost: ", sizeof req);
    cat_n(req, u.https ? host : u.host, sizeof req);
    /* images: only formats the engine decodes (no WebP/AVIF), so servers pick one of those */
    cat_n(req, f->kind == 1 ? "\r\nUser-Agent: BRO-weser\r\nAccept: text/css,*/*;q=0.1\r\nConnection: close\r\n\r\n"
             : f->kind == 2 ? "\r\nUser-Agent: BRO-weser\r\nAccept: image/png,image/jpeg,image/gif,image/bmp;q=0.9,*/*;q=0.1\r\nConnection: close\r\n\r\n"
                            : "\r\nUser-Agent: BRO-weser\r\nAccept: */*\r\nConnection: close\r\n\r\n", sizeof req);
    if(tcp_send(f->sock, req, ui_len(req)) != 0){ fetch_done(f, 0, NULL, 0); return; }
    f->cap = 64 * 1024;
    f->buf = kmalloc(f->cap + 1);
    if(!f->buf){ fetch_done(f, 0, NULL, 0); return; }
    f->len = f->hdr = f->clen = 0; f->have_clen = 0;
    f->tlast = timer_ticks();
    f->state = F_RECV;
}

/* A header of f's response (name lower case, with the colon). */
static int fetch_header(fetch_t *f, const char *name, char *out, u32 max){
    u32 nl = ui_len(name);
    for(u32 i = 0; i + nl < f->hdr; i++){
        if(i > 0 && f->buf[i - 1] != '\n') continue;
        u32 k = 0;
        while(k < nl && lower(f->buf[i + k]) == name[k]) k++;
        if(k < nl) continue;
        u32 p = i + nl, n = 0;
        while(p < f->hdr && f->buf[p] == ' ') p++;
        while(p < f->hdr && f->buf[p] != '\r' && f->buf[p] != '\n' && n + 1 < max) out[n++] = (char)f->buf[p++];
        out[n] = 0;
        return 1;
    }
    return 0;
}

static void fetch_finish(fetch_t *f){
    if(f->hdr == 0 || f->len < 12 || !starts((const char *)f->buf, "HTTP/")){ fetch_done(f, 0, NULL, 0); return; }
    int code = (f->buf[9] - '0') * 100 + (f->buf[10] - '0') * 10 + (f->buf[11] - '0');
    f->status = (u32)code;
    char val[URL_MAX];
    if(code >= 300 && code < 400 && fetch_header(f, "location:", val, sizeof val) && ++f->redirects <= MAX_REDIRECTS){
        char next[URL_MAX];
        resolve(f->url, val, next, sizeof next);
        ui_copy(f->url, next, URL_MAX);
        tcp_close(f->sock); f->sock = -1;
        kfree(f->buf); f->buf = NULL;
        f->state = F_START;
        return;
    }
    u32 blen = f->len - f->hdr;
    if(code >= 300 || blen == 0){ fetch_done(f, 0, NULL, 0); return; }
    if(f->kind == 1){
        char ct[80] = "text/css";
        fetch_header(f, "content-type:", ct, sizeof ct);
        for(char *q = ct; *q; q++) if(starts(q, "javascript") || starts(q, "html")){ fetch_done(f, 0, NULL, 0); return; }
    }
    if(f->kind != 4) cache_put(f->url, f->buf + f->hdr, blen);   /* video chunks would only push pages out */
    fetch_done(f, 1, f->buf + f->hdr, blen);
}

/* Reads what has arrived on f, without waiting. */
static void fetch_recv(fetch_t *f){
    for(int rounds = 0; rounds < 64; rounds++){
        if(f->len == f->cap){
            if(f->cap >= BODY_MAX) break;
            u32 nc = f->cap * 2 > BODY_MAX ? BODY_MAX : f->cap * 2;
            u8 *g = kmalloc(nc + 1);
            if(!g) break;
            memcpy(g, f->buf, f->len);
            kfree(f->buf);
            f->buf = g; f->cap = nc;
        }
        int got = tcp_recv(f->sock, f->buf + f->len, f->cap - f->len < 65536 ? f->cap - f->len : 65536, 0);
        if(got <= 0) break;
        f->len += (u32)got;
        f->tlast = timer_ticks();
    }
    if(f->hdr == 0){
        for(u32 i = 0; i + 3 < f->len; i++)
            if(f->buf[i] == '\r' && f->buf[i + 1] == '\n' && f->buf[i + 2] == '\r' && f->buf[i + 3] == '\n'){
                f->hdr = i + 4;
                char v[16];
                if(fetch_header(f, "content-length:", v, sizeof v)){
                    f->clen = 0;
                    for(char *q = v; *q >= '0' && *q <= '9'; q++) f->clen = f->clen * 10 + (u32)(*q - '0');
                    f->have_clen = 1;
                    /* make room for all of it now: no copying as it grows */
                    u32 want = f->hdr + f->clen;
                    if(want > f->cap && want <= BODY_MAX){
                        u8 *g = kmalloc(want + 1);
                        if(g){ memcpy(g, f->buf, f->len); kfree(f->buf); f->buf = g; f->cap = want; }
                    }
                }
                break;
            }
    }
    u64 now = timer_ticks();
    int done = tcp_closed(f->sock) || f->len >= BODY_MAX || (f->have_clen && f->hdr && f->len >= f->hdr + f->clen);
    if(done){ fetch_finish(f); return; }
    /* a script's fetch() may wait on a slow server (the proxy resolving a video): longer */
    if(f->len == 0 && now - f->tlast > (f->kind == 4 ? 3000u : (u64)FIRST_BYTE_TICKS)){ fetch_done(f, 0, NULL, 0); return; }
    if(f->len > 0 && now - f->tlast > IDLE_TICKS){ if(f->hdr) fetch_finish(f); else fetch_done(f, 0, NULL, 0); }
}

/* Hands one finished fetch to Husser (when it is free), then frees the slot. */
static int fetch_deliver(void){
    if(husser_busy()) return 0;
    for(int i = 0; i < FETCH_SLOTS; i++){
        fetch_t *f = &fslot[i];
        if(f->state != F_DONE) continue;
        if(f->kind == 4){
            husser_fetch_res(f->idx, f->status, f->ok ? f->body : NULL, f->ok ? f->blen : 0);
        } else if(f->ok){
            husser_res(f->kind, f->idx, f->body, f->blen);
            if(f->kind == 1) css_done++; else if(f->kind == 2) img_done++; else js_done++;
            if(f->cached) cache_hits++;
        } else husser_res(f->kind, f->idx, NULL, RES_FAIL_LEN);
        if(f->cached){ f->buf = NULL; f->cached = 0; }
        fetch_free(f);
        return 1;
    }
    return 0;
}

/* One step of every fetch; called every tick. */
static void fetch_step(void){
    /* start queued needs in free slots (a remembered one is "done" at once) */
    for(int i = 0; i < FETCH_SLOTS && npend > 0; i++){
        fetch_t *f = &fslot[i];
        if(f->state != F_FREE) continue;
        f->kind = pend[0].kind; f->idx = pend[0].idx;
        resolve(cur_url, pend[0].url, f->url, sizeof f->url);
        for(int k = 1; k < npend; k++) pend[k - 1] = pend[k];
        npend--;
        f->sock = -1; f->buf = NULL; f->redirects = 0; f->cached = 0; f->status = 0;
        if(f->kind != 4) fetch_total++;          /* a page's own fetches are not "loading the page" */
        if(browser_adblock && ad_url(f->url)){ ads_blocked++; fetch_done(f, 0, NULL, 0); continue; }
        int hit = f->kind == 4 ? -1 : cache_find(f->url);
        if(hit >= 0){ f->cached = 1; fetch_done(f, 1, rcache[hit].data, rcache[hit].len); continue; }
        f->state = F_START;
    }
    int connected = 0;
    for(int i = 0; i < FETCH_SLOTS; i++){
        fetch_t *f = &fslot[i];
        if(f->state == F_START && connected < 2){ fetch_connect(f); connected++; }   /* a couple of handshakes a tick */
    }
    /* read for up to a tick (three in ultra mode) while data keeps coming: the receive
       windows refill as they are drained, so the downloads do not wait for the next tick */
    u64 until = timer_ticks() + (ultra_mode() ? 3 : 1);
    for(;;){
        net_poll();
        u32 before = 0, after = 0;
        for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state == F_RECV) before += fslot[i].len;
        for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state == F_RECV) fetch_recv(&fslot[i]);
        for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state == F_RECV) after += fslot[i].len;
        if(after == before || timer_ticks() >= until) break;
    }
    fetch_deliver();
}

/* The status line while a page's resources come in: "Loading 7 of 23: images from host..." */
static void fetch_status(void){
    if(ld != LD_IDLE || !fetch_total) return;
    int left = fetch_active();
    /* a page's own fetch() calls (a video streaming) are not "loading the page" */
    for(int i = 0; i < FETCH_SLOTS; i++) if(fslot[i].state != F_FREE && fslot[i].kind == 4) left--;
    for(int k = 0; k < npend; k++) if(pend[k].kind == 4) left--;
    if(left <= 0) return;
    int npage = 0;
    for(int k = 0; k < npend; k++) if(pend[k].kind != 4) npage++;
    int inflight = left - npage;               /* slots busy (fetching or waiting for Husser) */
    int done = fetch_total - inflight;          /* fetch_total counts started ones */
    if(done < 0) done = 0;
    char m[128] = "Loading ", n1[12] = "", n2[12] = "";
    ui_num((u32)done, n1);
    ui_num((u32)(fetch_total + npage), n2);
    cat_n(m, n1, sizeof m); cat_n(m, " of ", sizeof m); cat_n(m, n2, sizeof m);
    cat_n(m, " (", sizeof m);
    char a[8] = ""; ui_num((u32)inflight, a);
    cat_n(m, a, sizeof m);
    cat_n(m, " at once) from ", sizeof m);
    for(int i = 0; i < FETCH_SLOTS; i++) if((fslot[i].state == F_RECV || fslot[i].state == F_START) && fslot[i].kind != 4){
        url_t u; if(parse_url(fslot[i].url, &u) == 0){ cat_n(m, u.host, sizeof m); break; }
    }
    cat_n(m, "...", sizeof m);
    ui_copy(status, m, sizeof status);
}

/* One step of loading; called every tick. */
static void ld_step(void){
    if(ld == LD_START){ ld = LD_CONNECT; return; }   /* draw one frame of "Connecting" first */
    if(ld == LD_CONNECT){ ld_connect(); return; }
    if(ld == LD_RECV) ld_recv();
}

/* ---- navigation ---- */

static void back(void){
    if(nhist == 0){ ui_copy(status, "No earlier page.", sizeof status); return; }
    nhist--;
    char u[URL_MAX];
    ui_copy(u, hist[nhist], URL_MAX);
    go(u, 0, hist_scroll[nhist]);
}

static void reload(void){
    if(cur_url[0]) go(cur_url, 0, scroll);
}

/* Scrolls the page to v pixels from its top, as far as it goes. */
static void scroll_to(int v){
    const volatile husser_shm_t *h = husser_header();
    int maxs = h ? (int)h->page_h - (int)area_of(last_body).h : 0;
    if(v > maxs) v = maxs;
    if(v < 0) v = 0;
    if(v == scroll) return;
    scroll = v;
    husser_paint((u32)scroll);
}

static void follow_number(int n){
    /* links are shown as [1], [2]... from 1; the engine numbers them from 0 */
    if(n < 1){ ui_copy(status, "No such link.", sizeof status); return; }
    const volatile husser_shm_t *h = husser_header();
    if(!h){ ui_copy(status, "No page loaded.", sizeof status); return; }
    waiting_link = n - 1;
    waiting_link_seq = h->link_url_seq;
    husser_link_url((u32)(n - 1));   /* b_tick picks the answer up */
    return;
}

/* ---- layout on screen ---- */

#define BAR_H 44
static rect_t back_btn(rect_t b){ return (rect_t){ b.x, b.y, 96, BAR_H }; }
static rect_t reload_btn(rect_t b){ return (rect_t){ b.x + 104, b.y, 112, BAR_H }; }
static rect_t go_btn(rect_t b){ return (rect_t){ b.x + b.w - 104, b.y, 104, BAR_H }; }
static rect_t addr_box(rect_t b){ return (rect_t){ b.x + 224, b.y, b.w - 224 - 112, BAR_H }; }
static rect_t area_of(rect_t b){
    if(b.h < BAR_H + 12 + 36 + 8) return (rect_t){ b.x, b.y, b.w, 8 };
    return (rect_t){ b.x, b.y + BAR_H + 12, b.w, b.h - BAR_H - 12 - 36 };
}

static void button(rect_t r, const char *label, u32 bg, u32 fg){
    ui_rrect(r.x, r.y, r.w, r.h, 10, bg);
    ui_text(r.x + (r.w - ui_text_w(label, UI_SCALE)) / 2, r.y + (r.h - UI_CH) / 2, label, fg, UI_SCALE);
}

/* Closing the window gives all its memory back: Husser exits, passing the page on. */
static void b_close(void){
    ld_cleanup();
    fetch_cancel_all();
    media_close();
    ld_kind = 0;
    css_done = 0;
    img_done = 0;
    js_done = 0; cache_hits = 0;
    husser_close();
    have_page = 0;
    last_body.w = 0;
    cur_url[0] = 0;
    addr[0] = 0; addr_len = 0;
    nhist = 0;
    scroll = 0;
    status[0] = 0;
    last_hx = -1; last_hy = -1;
}

/* Polls the shared region: the subresources Husser wants fetched, and the
 * navigation/link answers it gives back. Called every tick. */
static u32 fetch_seq_consumed = 0, last_fetch_id = 0;
static void poll_husser(void){
    const volatile husser_shm_t *h = husser_header();
    if(husser_alive()){
        if(h && have_page && !no_more_needs && ld == LD_IDLE && h->need_seq != need_seq_consumed){
            /* everything Husser lists that is not fetched or queued yet joins the queue */
            need_seq_consumed = h->need_seq;
            for(u32 k = 0; k < h->need_count && k < HUSSER_NEED_MAX; k++){
                u32 kind = h->need[k].kind, idx = h->need[k].idx;
                if(kind < 1 || kind > RES_KINDS || idx >= RES_IDX) continue;
                if(res_done[kind - 1][idx]) continue;
                if(starts((const char *)h->need[k].url, "data:")) continue;   /* husser handles data URLs itself */
                if(npend >= PEND_MAX) break;
                res_done[kind - 1][idx] = 1;                 /* once queued, never twice */
                pend[npend].kind = kind; pend[npend].idx = idx;
                ui_copy(pend[npend].url, (const char *)h->need[k].url, HUSSER_URL_NEED);
                npend++;
                resources_busy = 1;
            }
        }
        /* fetch() calls: their addresses are in the fetch queue, ids rising */
        if(h && have_page && h->fetch_seq != fetch_seq_consumed){
            static u8 ent[4096];
            int all = 1;
            for(u32 k = 0; k < h->fetch_count && k < HUSSER_FETCH_MAX; k++){
                husser_read(HUSSER_FETCHQ_OFF + (u64)k * 4096, ent, 4096);
                ent[4095] = 0;
                u32 id = (u32)ent[0] | (u32)ent[1] << 8 | (u32)ent[2] << 16 | (u32)ent[3] << 24;
                if(id <= last_fetch_id) continue;
                if(npend >= PEND_MAX){ all = 0; break; }
                last_fetch_id = id;
                pend[npend].kind = 4; pend[npend].idx = id;
                ui_copy(pend[npend].url, (const char *)ent + 4, URL_MAX);
                npend++;
            }
            if(all) fetch_seq_consumed = h->fetch_seq;
        }
        if(have_page && ld == LD_IDLE){ fetch_step(); fetch_status(); }
        if(h && have_page && ld == LD_IDLE && !husser_busy() && !fetch_active() && h->need_count == 0 && resources_busy){
            resources_busy = 0;
            loaded_status();                                 /* all stylesheets, scripts and images are in */
        }
        if(h && h->nav_seq != nav_seq_consumed){
            nav_seq_consumed = h->nav_seq;
            char u[URL_MAX];
            ui_copy(u, (const char *)h->nav_url, sizeof u);
            if(u[0]){
                char full[URL_MAX];
                if(starts(u, "about:")) ui_copy(full, u, sizeof full);
                else resolve(cur_url, u, full, sizeof full);
                go(full, 1, 0);
            }
            return;
        }
        if(waiting_link >= 0 && h && h->link_url_seq != waiting_link_seq){
            waiting_link = -1;
            if(!h->link_url[0]){ ui_copy(status, "No such link.", sizeof status); return; }
            char full[URL_MAX];
            if(starts((const char *)h->link_url, "about:")) ui_copy(full, (const char *)h->link_url, sizeof full);
            else resolve(cur_url, (const char *)h->link_url, full, sizeof full);
            go(full, 1, 0);
        }
    }
}

/* ---- <video> and MSE: the page appends coded media, Reel plays it ----
 * Husser runs the page's MediaSource/SourceBuffer/<video> objects and passes on what they
 * do, one command at a time (media_seq/media_ack); the bytes a page appends come through
 * the shared region. Here they go to Reel (reel_bridge.c), and every tick the position,
 * the buffered ranges and the rest go back for the page's script to see. The picture is
 * drawn into the <video> element's box, over the page Husser painted. */
static int media_open = 0;
static u8 *media_buf = NULL;
static void serial_log_line(const char *a, const char *b){
    for(const char *q = "browser: "; *q; q++) serial_putc(*q);
    for(const char *q = a; *q; q++) serial_putc(*q);
    for(const char *q = b; *q; q++) serial_putc(*q);
    serial_putc('\n');
}
static u32 media_err_seq = 0;
static rect_t video_on_screen = { 0, 0, 0, 0 };   /* where the last picture went */

static void media_fail(volatile husser_shm_t *h, const char *why){
    u32 i = 0;
    for(; why[i] && i + 1 < sizeof h->ms_error; i++) h->ms_error[i] = why[i];
    h->ms_error[i] = 0;
    h->ms_error_seq++;
    serial_log_line("video: ", why);
}

static void media_close(void){
    if(media_open) reel_end(&app_browser);
    media_open = 0;
    video_on_screen.w = 0;
}

static void media_step(void){
    volatile husser_shm_t *h = husser_header_rw();
    if(!h || !husser_alive() || !have_page){ media_close(); return; }
    if(h->media_seq != h->media_ack){
        u32 cmd = h->media_cmd;
        int a = h->media_a, done = 1;
        if(cmd != M_CMD_OPEN && cmd != M_CMD_CLOSE && !media_open){ h->media_ack = h->media_seq; return; }
        switch(cmd){
        case M_CMD_OPEN:
            if(reel_begin(&app_browser) == 0){ media_open = 1; reel_error(&media_err_seq); }
            else media_fail(h, "the media process could not start (out of memory?)");
            break;
        case M_CMD_APPEND: {
            u32 len = h->media_len;
            if(len > HUSSER_MEDIA_BYTES) len = HUSSER_MEDIA_BYTES;
            if(!media_buf) media_buf = kmalloc(HUSSER_MEDIA_BYTES);
            if(!media_buf){ media_fail(h, "out of memory"); break; }
            husser_read(HUSSER_MEDIA_OFF, media_buf, len);
            if(reel_append(a, media_buf, len) != 0) done = 0;      /* Reel's queue is full: next tick */
            break; }
        case M_CMD_REMOVE: reel_remove(a, h->media_t0, h->media_t1); break;
        case M_CMD_ABORT:  reel_abort(a); break;
        case M_CMD_EOS:    reel_eos(); break;
        case M_CMD_PLAY:   reel_play(); break;
        case M_CMD_PAUSE:  reel_pause(); break;
        case M_CMD_SEEK:   reel_seek(h->media_t0); break;
        case M_CMD_VOLUME: reel_set_volume(a * ui_prefs.volume / 100); break;
        case M_CMD_CLOSE:  media_close(); break;
        }
        if(done) h->media_ack = h->media_seq;
    }
    if(media_open && !reel_owned_by(&app_browser)){ media_open = 0; media_fail(h, "the video player took the sound card"); }
    if(!media_open) return;
    if(!reel_alive()){ media_fail(h, "the media process stopped"); media_close(); return; }
    if(reel_tick()){
        u32 pw, ph;
        if(video_on_screen.w && reel_picture(&pw, &ph)) ui_redraw_rect(video_on_screen);
        else ui_redraw();
    }
    const volatile reel_shm_t *r = reel_header();
    h->ms_time_us = reel_time();
    h->ms_duration_us = reel_duration();
    h->ms_playing = (u32)reel_playing();
    h->ms_ended = (u32)reel_ended();
    h->ms_stalled = (u32)reel_stalled();
    h->ms_ready = (u32)reel_picture(NULL, NULL) || (r && r->has_audio && !r->has_video);
    h->ms_video_w = r ? r->video_w : 0;
    h->ms_video_h = r ? r->video_h : 0;
    for(int k = 0; k < 2; k++){
        h->ms_pending[k] = (u32)reel_append_pending(k);
        u32 n = r ? r->sb[k].nranges : 0;
        if(n > 8) n = 8;
        for(u32 i = 0; i < n * 2; i++) h->ms_ranges[k][i] = r->sb[k].ranges[i];
        h->ms_nranges[k] = n;
    }
    u32 es;
    const char *e = reel_error(&es);
    if(es != media_err_seq){ media_err_seq = es; media_fail(h, e); }
    h->ms_seq++;
}

/* What the window showed when it was last drawn: a change means it is drawn again. */
static struct { u32 frame, title, console, error, state; int alive, ld; u64 phase; } seen;

static void b_tick(void){
    if(!last_body.w) return;                                    /* not drawn yet: the page size is not known */
    if(!have_page && ld == LD_IDLE) go("about:start", 0, -1);
    ld_step();                                                  /* loading goes on while minimized */
    husser_pump();
    poll_husser();
    media_step();

    const volatile husser_shm_t *h = husser_header();
    int alive = husser_alive();
    if(h && alive && have_page){
        /* the page is laid out for the size of the window */
        static u32 asked_w, asked_h;
        rect_t area = area_of(last_body);
        u32 w = area.w > HUSSER_MAX_W ? HUSSER_MAX_W : area.w, hh = area.h > HUSSER_MAX_H ? HUSSER_MAX_H : area.h;
        if(h->frame_seq && (w != h->vw || hh != h->vh) && (w != asked_w || hh != asked_h || !husser_busy())){
            asked_w = w; asked_h = hh;
            husser_resize(w, hh);
        }
    }
    /* the caret blinks twice a second; the progress bar moves 30 times a second */
    u64 now = timer_ticks();
    u64 phase = loading() ? now / 3 : editing ? now / 50 : 0;
    if(h && (h->frame_seq != seen.frame || h->title_seq != seen.title || h->console_seq != seen.console ||
             h->js_error_seq != seen.error || h->state != seen.state)){
        seen.frame = h->frame_seq; seen.title = h->title_seq; seen.console = h->console_seq;
        seen.error = h->js_error_seq; seen.state = h->state;
        ui_redraw();
    }
    if(alive != seen.alive || ld != seen.ld || phase != seen.phase){
        seen.alive = alive; seen.ld = ld; seen.phase = phase;
        ui_redraw();
    }
}

static void b_draw(rect_t b, int focused){
    (void)focused;
    last_body = b;
    u64 now = timer_ticks();

    /* toolbar */
    rect_t bb = back_btn(b), rb = reload_btn(b), gb = go_btn(b), ab = addr_box(b);
    button(bb, "Back", hover_btn == 0 && nhist ? C_HOVER : C_CARD, nhist ? C_TEXT : C_DIM);
    button(rb, "Reload", hover_btn == 1 && cur_url[0] ? C_HOVER : C_CARD, cur_url[0] ? C_TEXT : C_DIM);
    if(loading()) button(gb, "Stop", hover_btn == 2 ? 0xC42B1C : 0x8E2A20, C_TEXT);
    else button(gb, "Go", ui_accent(), C_BAR);

    ui_rrect(ab.x, ab.y, ab.w, ab.h, 10, editing ? ui_accent() : C_CARD);
    ui_rrect(ab.x + 2, ab.y + 2, ab.w - 4, ab.h - 4, 9, editing ? C_CARD_HI : C_CARD);
    u32 tx = ab.x + 14, ty = ab.y + (ab.h - UI_CH) / 2;
    if(!editing && starts(addr, "https://")){
        /* a small padlock: the page came over a checked TLS connection */
        u32 lx = tx, ly = ty + 4;
        ui_fill((rect_t){ lx + 2, ly, 10, 3 }, C_GOOD);
        ui_fill((rect_t){ lx + 2, ly, 3, 8 }, C_GOOD);
        ui_fill((rect_t){ lx + 9, ly, 3, 8 }, C_GOOD);
        ui_fill((rect_t){ lx, ly + 8, 14, 12 }, C_GOOD);
        tx += 24;
    }
    /* show the end of a long address while typing, else its start */
    u32 room = (ab.x + ab.w - 14 - tx) / UI_CW;
    const char *shown = addr_len ? addr : "Search or type an address";
    u32 slen = ui_len(shown);
    char clip[URL_MAX];
    if(slen > room && room > 3){
        if(editing){ ui_copy(clip, shown + (slen - room), sizeof clip); }
        else { ui_copy(clip, shown, room - 2 < sizeof clip ? room - 2 : sizeof clip); cat_n(clip, "..", sizeof clip); }
        shown = clip;
    }
    ui_text(tx, ty, shown, addr_len ? C_TEXT : C_DIM, UI_SCALE);
    if(editing && ((now / 50) & 1) == 0){
        u32 x = tx + ui_len(shown) * UI_CW + 1;
        if(x + 3 < ab.x + ab.w) ui_fill((rect_t){ x, ty, 2, UI_CH }, ui_accent());
    }

    /* page: Husser paints it; we blit the viewport it shares with us */
    rect_t area = area_of(b);
    ui_fill(area, C_WIN);
    const volatile husser_shm_t *hv = husser_header();
    if(have_page && hv){
        if(!husser_alive()){
            /* Husser died: the page is gone, the desktop is fine */
            ui_text(area.x + 24, area.y + 40, "Husser stopped", C_TEXT, UI_SCALE);
            ui_text(area.x + 24, area.y + 96, "This page crashed the Husser renderer; the desktop is fine.", C_MUTED, 1);
            ui_text(area.x + 24, area.y + 120, "Close this window and open Browser again to try once more.", C_MUTED, 1);
        } else if(hv->state == H_STATE_READY){
            husser_blit(area.x, area.y, area.w, area.h);        /* the last finished frame, while a new size is laid out */
            /* the page's <video>: the picture, kept in shape, inside the element's box */
            video_on_screen.w = 0;
            if(media_open && hv->video_w > 0 && hv->video_h > 0){
                const volatile reel_shm_t *r = reel_header();
                u32 bw = (u32)hv->video_w, bh = (u32)hv->video_h;
                u32 vw = r && r->video_w ? r->video_w : bw, vh = r && r->video_h ? r->video_h : bh;
                u32 dw = bw, dh = (u32)((u64)bw * vh / vw);
                if(dh > bh){ dh = bh; dw = (u32)((u64)bh * vw / vh); }
                reel_set_size(dw, dh, !reel_playing());
                i32 vx = (i32)area.x + hv->video_x + (i32)(bw - dw) / 2, vy = (i32)area.y + hv->video_y + (i32)(bh - dh) / 2;
                fb_set_clip(area.x, area.y, area.w, area.h);
                reel_blit(vx, vy, dw, dh);
                fb_set_clip(b.x, b.y, b.w, b.h);
                /* the visible part, for redrawing just the picture next time */
                i32 x0 = vx > (i32)area.x ? vx : (i32)area.x, y0 = vy > (i32)area.y ? vy : (i32)area.y;
                i32 x1 = vx + (i32)dw < (i32)(area.x + area.w) ? vx + (i32)dw : (i32)(area.x + area.w);
                i32 y1 = vy + (i32)dh < (i32)(area.y + area.h) ? vy + (i32)dh : (i32)(area.y + area.h);
                if(x1 > x0 && y1 > y0) video_on_screen = (rect_t){ (u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0) };
            }
        } else if(hv->state == H_STATE_BOOT){
            ui_text(area.x + 24, area.y + 40, "Loading...", C_MUTED, UI_SCALE);
        } else {                                                /* H_STATE_FAILED */
            ui_text(area.x + 24, area.y + 40, "This page could not be rendered.", C_TEXT, UI_SCALE);
            if(hv->js_error[0]) ui_text(area.x + 24, area.y + 96, (const char *)hv->js_error, C_MUTED, 1);
        }
        int pgh = (int)hv->page_h;
        int maxs = pgh - (int)area.h;
        if(maxs < 0) maxs = 0;
        if(scroll > maxs) scroll = maxs;
        if(scroll < 0) scroll = 0;
        if(pgh > (int)area.h && hv->state == H_STATE_READY){
            int bar_h = (int)area.h * (int)area.h / pgh;
            if(bar_h < 24) bar_h = 24;
            int bar_y = (int)area.y + (int)((long long)scroll * ((int)area.h - bar_h) / (maxs ? maxs : 1));
            ui_rrect(area.x + area.w - 8, (u32)bar_y, 5, (u32)bar_h, 2, C_DIM);
        }
    }

    /* progress bar along the top of the page while loading */
    if(loading()){
        u32 w = area.w;
        ui_fill((rect_t){ area.x, area.y, w, 3 }, C_SEP);
        int npage = 0;
        for(int k = 0; k < npend; k++) if(pend[k].kind != 4) npage++;
        int left = page_fetches(), all = fetch_total + npage;     /* started + waiting; left: busy + waiting */
        if(ld == LD_IDLE && all > 0){
            /* the page is here: how many of its resources are in */
            int got = all - left; if(got < 0) got = 0;
            u32 pw = (u32)((u64)w * (u32)got / (u32)all);
            if(pw < 8) pw = 8;
            ui_fill((rect_t){ area.x, area.y, pw, 3 }, ui_accent());
        } else if(ld == LD_RECV && ld_have_clen && ld_hdr && ld_clen){
            u32 got = ld_len - ld_hdr;
            u32 pw = (u32)((u64)w * (got > ld_clen ? ld_clen : got) / ld_clen);
            if(pw < 8) pw = 8;
            ui_fill((rect_t){ area.x, area.y, pw, 3 }, ui_accent());
        } else {
            /* not known how long: a segment sweeps across */
            int seg = (int)w / 4;
            int pos = (int)((now - ld_t0) * 14 % (u64)(w + (u32)seg)) - seg;
            int x0 = pos < 0 ? 0 : pos, x1 = pos + seg > (int)w ? (int)w : pos + seg;
            if(x1 > x0) ui_fill((rect_t){ area.x + (u32)x0, area.y, (u32)(x1 - x0), 3 }, ui_accent());
        }
    }

    /* footer: page title, script messages, then the status */
    char foot[200] = "";
    if(have_page && hv && hv->title[0]){ cat_n(foot, (const char *)hv->title, sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
    if(ld == LD_IDLE && hv){
        if(hv->js_error[0]){ cat_n(foot, "Script error: ", sizeof foot); cat_n(foot, (const char *)hv->js_error, sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
        else if(hv->console[0]){ cat_n(foot, "console: ", sizeof foot); cat_n(foot, (const char *)hv->console, sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
    }
    if(num_len){ cat_n(foot, "Link ", sizeof foot); cat_n(foot, num_buf, sizeof foot); cat_n(foot, " (Enter)  ", sizeof foot); }
    cat_n(foot, status, sizeof foot);
    u32 fl = ui_len(foot);
    if(fl >= 5 && same(foot + fl - 5, "  |  ")) foot[fl - 5] = 0;     /* nothing after the last part */
    u32 fmax = b.w / 8;                       /* the small font is 8 pixels wide */
    if(ui_len(foot) > fmax && fmax > 2){ foot[fmax - 2] = '.'; foot[fmax - 1] = '.'; foot[fmax] = 0; }
    ui_text(b.x + 4, b.y + b.h - 26, foot, loading() ? C_TEXT : C_MUTED, 1);
}

static void b_move(rect_t b, int x, int y){
    int hb = ui_contains(back_btn(b), x, y) ? 0 : ui_contains(reload_btn(b), x, y) ? 1 :
             ui_contains(go_btn(b), x, y) ? 2 : -1;
    if(hb != hover_btn){ hover_btn = hb; ui_redraw(); }
    rect_t area = area_of(b);
    if(!have_page || !ui_contains(area, x, y)) return;
    int hx = x - (int)area.x, hy = y - (int)area.y;
    if(hx == last_hx && hy == last_hy) return;
    last_hx = hx; last_hy = hy;
    husser_move(hx, hy);
}

static void b_click(rect_t b, int x, int y){
    if(ui_contains(back_btn(b), x, y)){ back(); return; }
    if(ui_contains(reload_btn(b), x, y)){ reload(); return; }
    if(ui_contains(go_btn(b), x, y)){
        if(loading()){ stop(); return; }
        if(addr_len) go(addr, 1, -1);
        return;
    }
    if(ui_contains(addr_box(b), x, y)){
        if(!editing){ editing = 1; addr_len = 0; addr[0] = 0; }    /* start fresh, like selecting all */
        return;
    }
    if(editing){ editing = 0; ui_copy(addr, cur_url, sizeof addr); addr_len = (int)ui_len(addr); }

    rect_t area = area_of(b);
    if(!have_page || !ui_contains(area, x, y)) return;
    husser_click(x - (int)area.x, y - (int)area.y);
    /* the click runs inside Husser; a link it wants followed comes back via nav_seq */
}

static int b_key(int c){
    if(editing){
        if(c == 27){ editing = 0; ui_copy(addr, cur_url, sizeof addr); addr_len = (int)ui_len(addr); return 1; }
        if(c == '\n'){ editing = 0; if(addr_len) go(addr, 1, -1); return 1; }
        if(c == '\b'){ if(addr_len) addr[--addr_len] = 0; return 1; }
        if(c >= 32 && c <= 126 && addr_len < URL_MAX - 1){ addr[addr_len++] = (char)c; addr[addr_len] = 0; return 1; }
        return 1;
    }
    if(c == 27 && loading()){ stop(); return 1; }
    /* an input box on the page has focus: keys go to Husser, and it decides */
    {
        const volatile husser_shm_t *h = husser_header();
        if(h && h->input_focus){
            if(c == 27){ husser_key(27); return 1; }
            husser_key((u32)(c & 0x7F));
            return 1;
        }
    }
    if(c >= '0' && c <= '9'){
        if(num_len < 3){ num_buf[num_len++] = (char)c; num_buf[num_len] = 0; }
        return 1;
    }
    if(c == '\n' && num_len){
        unsigned n = 0;
        for(int i = 0; i < num_len; i++) n = n * 10 + (unsigned)(num_buf[i] - '0');
        num_len = 0; num_buf[0] = 0;
        follow_number((int)n);
        return 1;
    }
    if(c == '\b'){
        if(num_len){ num_buf[--num_len] = 0; return 1; }
        back();
        return 1;
    }
    if(c == KEY_UP){ scroll_to(scroll - SCROLL_STEP); return 1; }
    if(c == KEY_DOWN){ scroll_to(scroll + SCROLL_STEP); return 1; }
    if(c == KEY_PGUP){ scroll_to(scroll - ((int)area_of(last_body).h - 48)); return 1; }
    if(c == KEY_PGDN){ scroll_to(scroll + ((int)area_of(last_body).h - 48)); return 1; }
    if(c == KEY_HOME){ scroll_to(0); return 1; }
    if(c == KEY_END){ scroll_to(1 << 30); return 1; }
    /* any other letter starts typing an address, like a real address bar */
    if(c >= 32 && c <= 126){
        editing = 1; addr_len = 0; addr[0] = 0;
        addr[addr_len++] = (char)c; addr[addr_len] = 0;
        return 1;
    }
    return 0;
}

static void b_scroll(rect_t b, int x, int y, int dz){
    (void)b; (void)x; (void)y;
    scroll_to(scroll + dz * 2 * SCROLL_STEP);
}

const app_t app_browser = {
    .name = "Browser",
    .icon = UI_ICO_BROWSER,
    .close = b_close,
    .draw = b_draw,
    .key = b_key,
    .click = b_click,
    .move = b_move,
    .tick = b_tick,
    .scroll = b_scroll,
};
