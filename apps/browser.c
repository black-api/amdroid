/* Browser: a web browser. It loads pages over HTTP (and HTTPS through the host's proxy,
 * tools/https_proxy.py) and shows them with the HTML, CSS and JavaScript engines in
 * src/kernel/web and src/kernel/js.
 *
 * Loading runs a step at a time from the draw loop, so the desktop keeps moving while a
 * page arrives: a progress bar runs under the toolbar, Go turns into Stop, and the footer
 * says what is happening. A page that cannot be loaded gets an error page with Try again. */
#include "amdroid/ui.h"
#include "amdroid/net.h"
#include "amdroid/heap.h"
#include "amdroid/keyboard.h"
#include "amdroid/web.h"
#include "amdroid/fb.h"
#include "amdroid/timer.h"

#define URL_MAX     1024           /* stylesheet addresses can be long (Wikipedia: ~500) */
#define HOST_MAX    64
#define PATH_MAX_B  1024
#define HIST_MAX    16
#define BODY_MAX    (1024 * 1024)
#define SCROLL_STEP 48
#define MAX_REDIRECTS 5
#define FIRST_BYTE_TICKS 3000     /* 30 s for the first byte (the proxy may be fetching) */
#define IDLE_TICKS       1000     /* 10 s of silence after that ends the page */

#define PROXY_IP   0x0A000202u    /* 10.0.2.2: the host, as seen from the guest */
#define PROXY_PORT 8888

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
static int page_h = 0;              /* total height of the laid-out page */
static int have_page = 0;
static int laid_width = -1;
static rect_t last_body;
static void *hover_node = NULL;     /* the element under the pointer */
static void *focus_node = NULL;     /* the text box that gets typed keys */
static int hover_btn = -1;          /* toolbar button under the pointer */
static int b_focused = 0;           /* the window had focus at its last draw */
static u32 ticks_n = 0, drawn_at = 0;   /* frames: to tell when it is minimized (not drawn) */
extern const app_t app_browser;

/* A notification when the user is looking at something else. */
static void tell(const char *title, const char *body){
    int visible = b_focused && ticks_n - drawn_at <= 2;
    if(!visible) ui_notify(ui_app_index(&app_browser), title, body);
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
    "<html><head><title>New tab</title><style>"
    "body{margin:0;color:#c9d4dc}"
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
    "<p class=\"sub\">Type an address in the bar above and press Enter, or open one of these:</p></div>"
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
static u32 ld_len, ld_hdr, ld_clen;
static int ld_have_clen;
static u64 ld_t0, ld_tlast;
static char ld_host[HOST_MAX];        /* shown in the footer */
static int ld_kind;                   /* 0 the page, 1 one of its stylesheets */
static int css_next, css_total, css_done;
static u32 page_kb;                   /* for the final "Loaded" line */
static int page_code;
static int page_cut;

static void next_sheet(void);
static void loaded_status(void);

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

/* Shows html as the page for url. */
static int show(const char *url, const char *html, u32 len){
    web_set_url(url);
    if(web_load(html, len) != 0) return -1;
    if(ld_remember && !same(cur_url, url)) push_history();
    ui_copy(cur_url, url, URL_MAX);
    if(!editing){ ui_copy(addr, url, sizeof addr); addr_len = (int)ui_len(addr); }
    have_page = 1;
    laid_width = -1;                  /* laid out again at the next draw */
    scroll = ld_scroll >= 0 ? ld_scroll : 0;
    hover_node = NULL;                /* those pointed into the old page */
    focus_node = NULL;
    return 0;
}

static void fail(const char *why){
    static char page[2048];
    ld_cleanup();
    if(ld_kind == 1){ next_sheet(); return; }     /* a stylesheet that fails is left out */
    ui_copy(page, "<html><head><title>Can't open this page</title><style>"
                  "body{margin:24px}h1{color:#ffb547}p{color:#9aa7b0}a{color:#4cc2ff}"
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

/* Starts loading url. The page on screen stays until the new one arrives. */
static void go(const char *url, int remember, int scroll_to){
    ld_cleanup();
    ld_kind = 0;
    css_next = css_total = css_done = 0;
    char full[URL_MAX];
    typed_url(url, full, sizeof full);
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
    if(ld == LD_IDLE) return;
    ld_cleanup();
    if(ld_kind == 1){                    /* stop the stylesheets: the page itself is here */
        css_next = css_total;
        ld_kind = 0;
        ui_copy(status, "Stopped (some styles were not loaded).", sizeof status);
        return;
    }
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
                            : "\r\nUser-Agent: BRO-weser\r\nAccept: text/html,text/plain\r\nConnection: close\r\n\r\n", sizeof req);
    if(tcp_send(ld_sock, req, ui_len(req)) != 0){ fail("Sending the request failed."); return; }

    ld_buf = kmalloc(BODY_MAX + 1);
    if(!ld_buf){ fail("Out of memory."); return; }
    ld_len = ld_hdr = ld_clen = 0;
    ld_have_clen = 0;
    ld_tlast = timer_ticks();
    ld = LD_RECV;
    if(ld_kind == 1) return;                 /* "Loading styles..." stays */
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
    if(ld_kind == 1){
        /* a stylesheet: add it and lay the page out again */
        char ct[80] = "text/css";
        header("content-type:", ct, sizeof ct);
        int not_css = 0;
        for(char *q = ct; *q; q++) if(starts(q, "javascript") || starts(q, "html")) not_css = 1;
        if(code < 300 && blen && !not_css){ web_add_css(body, blen); laid_width = -1; css_done++; }
        ld_cleanup();
        next_sheet();
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
    int html = 0, text = 0;
    for(char *q = ctype; *q; q++) if(starts(q, "html") || starts(q, "xml")) html = 1;
    if(!html && starts(ctype, "text/")) text = 1;
    if(!html && !text){
        char m[128] = "This is a file of type ";
        cat_n(m, ctype, sizeof m);
        cat_n(m, ", which Browser cannot show.", sizeof m);
        fail(m);
        return;
    }

    int r;
    if(text){
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
    page_cut = ld_len >= BODY_MAX;
    ld_cleanup();
    /* then the page's linked stylesheets, one after another */
    css_next = 0;
    css_done = 0;
    css_total = web_stylesheet_count();
    next_sheet();
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
    cat_n(m, " in ", sizeof m);
    cat_n(m, sec, sizeof m); cat_n(m, ".", sizeof m); cat_n(m, ten, sizeof m); cat_n(m, " s", sizeof m);
    if(page_cut) cat_n(m, " (cut off at 1 MB)", sizeof m);
    ui_copy(status, m, sizeof status);
}

/* Starts the next linked stylesheet, or finishes the page when there are none left. */
static void next_sheet(void){
    while(css_next < css_total){
        const char *href = web_stylesheet(css_next++);
        char u[URL_MAX];
        url_t pu;
        if(!href) continue;
        resolve(cur_url, href, u, sizeof u);
        if(parse_url(u, &pu) != 0) continue;
        ui_copy(ld_url, u, URL_MAX);
        ui_copy(ld_host, pu.host, sizeof ld_host);
        ld_kind = 1;
        ld_redirects = 0;
        ld = LD_START;
        char a[8] = "", b[8] = "";
        ui_num((u32)css_next, a);
        ui_num((u32)css_total, b);
        ui_copy(status, "Loading styles ", sizeof status);
        cat_n(status, a, sizeof status); cat_n(status, " of ", sizeof status); cat_n(status, b, sizeof status);
        cat_n(status, " from ", sizeof status); cat_n(status, pu.host, sizeof status); cat_n(status, "...", sizeof status);
        return;
    }
    ld_kind = 0;
    loaded_status();
    tell("Page loaded", web_title()[0] ? web_title() : cur_url);
}

/* Reads whatever has arrived, without waiting. */
static void ld_recv(void){
    /* read for up to 2 ticks (20 ms) per frame: fast, and the desktop stays smooth */
    u64 now = timer_ticks(), until = now + 2;
    for(;;){
        net_poll();
        u32 room = BODY_MAX - ld_len;
        if(room == 0) break;
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
    int done = tcp_closed(ld_sock) || ld_len >= BODY_MAX ||
               (ld_have_clen && ld_hdr && ld_len >= ld_hdr + ld_clen);
    if(done){ ld_finish(); return; }
    if(ld_len == 0 && now - ld_tlast > FIRST_BYTE_TICKS){ fail("The server did not answer in time."); return; }
    if(ld_len > 0 && now - ld_tlast > IDLE_TICKS){
        if(ld_hdr) ld_finish();                 /* show what came */
        else fail("The server stopped sending.");
    }
}

/* One step of loading; called every frame. */
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

static void follow_index(int idx){
    const char *href = web_link(idx);
    if(!href){ ui_copy(status, "No such link.", sizeof status); return; }
    if(starts(href, "javascript:")) return;
    if(starts(href, "mailto:")){ ui_copy(status, "Mail links are not supported.", sizeof status); return; }
    char u[URL_MAX];
    resolve(cur_url, href, u, URL_MAX);
    go(u, 1, -1);
}

static void follow_number(int n){
    /* links are shown as [1], [2]... from 1; the engine numbers them from 0 */
    if(n < 1){ ui_copy(status, "No such link.", sizeof status); return; }
    follow_index(n - 1);
}

/* ---- layout on screen ---- */

#define BAR_H 44
static rect_t back_btn(rect_t b){ return (rect_t){ b.x, b.y, 96, BAR_H }; }
static rect_t reload_btn(rect_t b){ return (rect_t){ b.x + 104, b.y, 112, BAR_H }; }
static rect_t go_btn(rect_t b){ return (rect_t){ b.x + b.w - 104, b.y, 104, BAR_H }; }
static rect_t addr_box(rect_t b){ return (rect_t){ b.x + 224, b.y, b.w - 224 - 112, BAR_H }; }
static rect_t area_of(rect_t b){ return (rect_t){ b.x, b.y + BAR_H + 12, b.w, b.h - BAR_H - 12 - 36 }; }

static void button(rect_t r, const char *label, u32 bg, u32 fg){
    ui_rrect(r.x, r.y, r.w, r.h, 10, bg);
    ui_text(r.x + (r.w - ui_text_w(label, UI_SCALE)) / 2, r.y + (r.h - UI_CH) / 2, label, fg, UI_SCALE);
}

/* Closing the window gives all its memory back. */
static void b_close(void){
    ld_cleanup();
    ld_kind = 0;
    css_next = css_total = 0;
    web_release();
    have_page = 0;
    cur_url[0] = 0;
    addr[0] = 0; addr_len = 0;
    nhist = 0;
    scroll = 0;
    laid_width = -1;
    hover_node = focus_node = NULL;
    status[0] = 0;
}

static void b_open(void){
    if(!have_page && ld == LD_IDLE) go("about:start", 0, -1);
}

static void b_tick(void){
    ticks_n++;
    if(!have_page && ld == LD_IDLE) go("about:start", 0, -1);
    web_tick();                                                 /* script timers that are due */
    ld_step();                                                  /* loading goes on while minimized */
}

static void b_draw(rect_t b, int focused){
    b_focused = focused;
    drawn_at = ticks_n;
    last_body = b;
    u64 now = timer_ticks();

    /* toolbar */
    rect_t bb = back_btn(b), rb = reload_btn(b), gb = go_btn(b), ab = addr_box(b);
    button(bb, "Back", hover_btn == 0 && nhist ? C_HOVER : C_CARD, nhist ? C_TEXT : C_DIM);
    button(rb, "Reload", hover_btn == 1 && cur_url[0] ? C_HOVER : C_CARD, cur_url[0] ? C_TEXT : C_DIM);
    if(ld != LD_IDLE) button(gb, "Stop", hover_btn == 2 ? 0xC42B1C : 0x8E2A20, C_TEXT);
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

    /* page */
    rect_t area = area_of(b);
    ui_fill(area, C_WIN);
    if(web_dirty()){ web_clear_dirty(); laid_width = -1; }   /* a script changed the page */
    if(have_page){
        if(laid_width != (int)area.w){
            page_h = web_layout((int)area.w);
            laid_width = (int)area.w;
        }
        int maxs = page_h - (int)area.h;
        if(maxs < 0) maxs = 0;
        if(scroll > maxs) scroll = maxs;
        if(scroll < 0) scroll = 0;

        int n = 0;
        const web_cmd_t *cmds = web_cmds(&n);
        for(int i = 0; i < n; i++){
            const web_cmd_t *c = &cmds[i];
            int y = (int)area.y + c->y - scroll;
            if(c->kind == WEB_CMD_RECT){
                /* clip the box to the page area so it cannot spill over the window */
                int x0 = (int)area.x + c->x, y0 = y;
                int x1 = x0 + c->w, y1 = y0 + c->h;
                if(x0 < (int)area.x) x0 = (int)area.x;
                if(y0 < (int)area.y) y0 = (int)area.y;
                if(x1 > (int)(area.x + area.w)) x1 = (int)(area.x + area.w);
                if(y1 > (int)(area.y + area.h)) y1 = (int)(area.y + area.h);
                if(x1 > x0 && y1 > y0) fb_fill_rect((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0), c->color);
                continue;
            }
            if(y < (int)area.y || y + c->h > (int)(area.y + area.h)) continue;   /* only whole lines */
            u32 x = (u32)((int)area.x + c->x);
            if((int)x + c->w < (int)area.x || (int)x > (int)(area.x + area.w)) continue;
            ui_text(x, (u32)y, c->text, c->color, (u32)c->scale);
            if(c->bold) ui_text(x + 1, (u32)y, c->text, c->color, (u32)c->scale);
            if(c->link >= 0 && c->dom == hover_node)                  /* underline the link under the pointer */
                ui_fill((rect_t){ x, (u32)(y + c->h - 2), (u32)c->w, 2 }, c->color);
        }
        if(page_h > (int)area.h){
            int bar_h = (int)area.h * (int)area.h / page_h;
            if(bar_h < 24) bar_h = 24;
            int bar_y = (int)area.y + (int)((long long)scroll * ((int)area.h - bar_h) / (maxs ? maxs : 1));
            ui_rrect(area.x + area.w - 8, (u32)bar_y, 5, (u32)bar_h, 2, C_DIM);
        }
    }

    /* progress bar along the top of the page while loading */
    if(ld != LD_IDLE){
        u32 w = area.w;
        ui_fill((rect_t){ area.x, area.y, w, 3 }, C_SEP);
        if(ld == LD_RECV && ld_have_clen && ld_hdr && ld_clen){
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
    if(have_page && web_title()[0]){ cat_n(foot, web_title(), sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
    if(ld == LD_IDLE){
        if(web_js_error()[0]){ cat_n(foot, "Script error: ", sizeof foot); cat_n(foot, web_js_error(), sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
        else if(web_console()[0]){ cat_n(foot, "console: ", sizeof foot); cat_n(foot, web_console(), sizeof foot); cat_n(foot, "  |  ", sizeof foot); }
    }
    if(num_len){ cat_n(foot, "Link ", sizeof foot); cat_n(foot, num_buf, sizeof foot); cat_n(foot, " (Enter)  ", sizeof foot); }
    cat_n(foot, status, sizeof foot);
    u32 fl = ui_len(foot);
    if(fl >= 5 && same(foot + fl - 5, "  |  ")) foot[fl - 5] = 0;     /* nothing after the last part */
    u32 fmax = b.w / 8;                       /* the small font is 8 pixels wide */
    if(ui_len(foot) > fmax && fmax > 2){ foot[fmax - 2] = '.'; foot[fmax - 1] = '.'; foot[fmax] = 0; }
    ui_text(b.x + 4, b.y + b.h - 26, foot, ld != LD_IDLE ? C_TEXT : C_MUTED, 1);
}

/* The element drawn under (x, y) in the page area, or NULL. */
static void *element_at(rect_t b, int x, int y){
    rect_t area = area_of(b);
    if(!have_page || !ui_contains(area, x, y)) return NULL;
    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    int px = x - (int)area.x, py = y - (int)area.y + scroll;
    void *hit = NULL;
    for(int i = 0; i < n; i++){
        const web_cmd_t *c = &cmds[i];
        if(c->kind != WEB_CMD_TEXT) continue;
        if(px >= c->x && px < c->x + c->w && py >= c->y && py < c->y + c->h) hit = (void *)c->dom;
    }
    return hit;
}

static void b_move(rect_t b, int x, int y){
    hover_btn = ui_contains(back_btn(b), x, y) ? 0 : ui_contains(reload_btn(b), x, y) ? 1 :
                ui_contains(go_btn(b), x, y) ? 2 : -1;
    void *now = element_at(b, x, y);
    if(now == hover_node) return;
    if(hover_node) web_dispatch(hover_node, "mouseout", NULL);
    hover_node = now;
    if(hover_node) web_dispatch(hover_node, "mouseover", NULL);
}

static void b_click(rect_t b, int x, int y){
    if(ui_contains(back_btn(b), x, y)){ back(); return; }
    if(ui_contains(reload_btn(b), x, y)){ reload(); return; }
    if(ui_contains(go_btn(b), x, y)){
        if(ld != LD_IDLE){ stop(); return; }
        if(addr_len) go(addr, 1, -1);
        return;
    }
    if(ui_contains(addr_box(b), x, y)){
        if(!editing){ editing = 1; addr_len = 0; addr[0] = 0; }    /* start fresh, like selecting all */
        focus_node = NULL;
        return;
    }
    if(editing){ editing = 0; ui_copy(addr, cur_url, sizeof addr); addr_len = (int)ui_len(addr); }

    rect_t area = area_of(b);
    if(!have_page || !ui_contains(area, x, y)) return;
    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    int px = x - (int)area.x, py = y - (int)area.y + scroll;
    for(int i = 0; i < n; i++){
        const web_cmd_t *c = &cmds[i];
        if(c->kind != WEB_CMD_TEXT) continue;
        if(px >= c->x && px < c->x + c->w && py >= c->y && py < c->y + c->h){
            /* click handlers run first; a handler that ran stops the default link action */
            web_click(c->dom);
            if(web_is_input(c->dom)){ focus_node = (void *)c->dom; return; }   /* a text box */
            focus_node = NULL;
            if(web_take_prevented()) return;           /* a handler called preventDefault */
            if(c->link >= 0) follow_index(c->link);
            return;
        }
    }
}

static int b_key(int c){
    if(editing){
        if(c == 27){ editing = 0; ui_copy(addr, cur_url, sizeof addr); addr_len = (int)ui_len(addr); return 1; }
        if(c == '\n'){ editing = 0; if(addr_len) go(addr, 1, -1); return 1; }
        if(c == '\b'){ if(addr_len) addr[--addr_len] = 0; return 1; }
        if(c >= 32 && c <= 126 && addr_len < URL_MAX - 1){ addr[addr_len++] = (char)c; addr[addr_len] = 0; return 1; }
        return 1;
    }
    if(c == 27 && ld != LD_IDLE){ stop(); return 1; }
    if(focus_node){
        if(c == 27){ focus_node = NULL; return 1; }
        if(web_key_input(focus_node, c)) return 1;
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
    if(c == KEY_UP){ scroll -= SCROLL_STEP; return 1; }
    if(c == KEY_DOWN){ scroll += SCROLL_STEP; return 1; }
    if(c == KEY_PGUP){ scroll -= (int)area_of(last_body).h - 48; return 1; }
    if(c == KEY_PGDN){ scroll += (int)area_of(last_body).h - 48; return 1; }
    /* any other letter starts typing an address, like a real address bar */
    if(c >= 32 && c <= 126){
        editing = 1; addr_len = 0; addr[0] = 0;
        addr[addr_len++] = (char)c; addr[addr_len] = 0;
        return 1;
    }
    return 0;
}

const app_t app_browser = {
    .name = "Browser",
    .icon = UI_ICO_BROWSER,
    .pinned = 0,
    .open = b_open,
    .close = b_close,
    .draw = b_draw,
    .key = b_key,
    .click = b_click,
    .move = b_move,
    .tick = b_tick,
};
