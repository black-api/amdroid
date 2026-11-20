/* host_main.c — render Am-Droid's web engine (HTML/CSS/layout/JS/paint) on
 * Linux to a PPM, so rendering can be iterated without booting QEMU. It drives
 * the exact same engine + painter that Husser runs in the OS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amdroid/types.h"
#include "amdroid/web.h"
#include "amdroid/js.h"
#include "amdroid/markdown.h"

/* painter.c (compiled in) */
void painter_target(u32 *buf, u32 w, u32 h);
void painter_clear(u32 c);
void painter_cmds(const web_cmd_t *cmds, int n, int scroll, const void *hover_node);

/* js.c GC: dummy symbol to satisfy the link; we override the scan top at runtime */
char stack_top[1];
void js_set_stack_top(unsigned long t);

static char g_dir[1024];

static char *read_all(const char *path, u32 *out_len){
    FILE *f = fopen(path, "rb");
    if(!f){ return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if(!b){ fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if(out_len) *out_len = (u32)got;
    return b;
}

static const char *g_page_url = "http://localhost/index.html";

/* the absolute URL of href on the page (like the browser's resolve()) */
static void absolute(char *dst, size_t cap, const char *href){
    while(*href == ' ' || *href == '\n' || *href == '\t') href++;
    if(strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0){ snprintf(dst, cap, "%s", href); }
    else {
        const char *b = g_page_url, *hs = strstr(b, "://");
        const char *pathp = hs ? strchr(hs + 3, '/') : NULL;
        size_t olen = pathp ? (size_t)(pathp - b) : strlen(b);
        if(href[0] == '/' && href[1] == '/'){ size_t sl = hs ? (size_t)(hs - b) + 1 : 5; snprintf(dst, cap, "%.*s%s", (int)sl, b, href); }
        else if(href[0] == '/') snprintf(dst, cap, "%.*s%s", (int)olen, b, href);
        else {
            size_t cut = olen + 1;
            if(pathp){ const char *q = pathp; size_t last = 0; for(; *q && *q != '?'; q++) if(*q == '/') last = (size_t)(q - b) + 1; cut = last; }
            else { snprintf(dst, cap, "%s/%s", b, href); goto done; }
            snprintf(dst, cap, "%.*s%s", (int)cut, b, href);
        }
    }
done:
    { char *h = strchr(dst, '#'); if(h) *h = 0; }
}

/* resolve an href to a local file: the mirror keeps each resource as _r/<fnv1a64 of its URL> */
static char g_last_abs[2048];
static void resolve(char *dst, size_t cap, const char *href){
    char *abs = g_last_abs; absolute(abs, sizeof g_last_abs, href);
    unsigned long long hsh = 1469598103934665603ull;
    for(const unsigned char *q = (const unsigned char *)abs; *q; q++){ hsh ^= *q; hsh *= 1099511628211ull; }
    snprintf(dst, cap, "%s/_r/%016llx", g_dir, hsh);
    FILE *f = fopen(dst, "rb");
    if(f){ fclose(f); return; }
    /* older mirrors: the URL's path under the page's directory */
    if(strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0){
        const char *p = strchr(href + 8, '/');
        snprintf(dst, cap, "%s%s", g_dir, p ? p : "/index.html");
        return;
    }
    if(href[0] == '/') snprintf(dst, cap, "%s%s", g_dir, href);
    else snprintf(dst, cap, "%s/%s", g_dir, href);
    { char *q = strchr(dst + strlen(g_dir), '?'); if(q) *q = 0; }
}

int main(int argc, char **argv){
    if(argc < 3){
        fprintf(stderr, "usage: %s page.html out.ppm [width] [maxheight]\n", argv[0]);
        return 2;
    }
    /* the GC scans the stack from the current frame up to here */
    js_set_stack_top((unsigned long)__builtin_frame_address(0) + 4096);

    const char *in = argv[1], *out = argv[2];
    int width   = argc > 3 ? atoi(argv[3]) : 1200;
    int maxh    = argc > 4 ? atoi(argv[4]) : 8000;

    /* directory of the input page, for relative subresources */
    { size_t L = strlen(in); if(L >= sizeof g_dir) L = sizeof g_dir - 1; memcpy(g_dir, in, L); g_dir[L] = 0; }
    char *slash = strrchr(g_dir, '/');
    if(slash) *slash = 0; else strcpy(g_dir, ".");

    u32 len = 0;
    char *html = read_all(in, &len);
    if(!html){ fprintf(stderr, "cannot read %s\n", in); return 1; }
    if(md_is_markdown_path(in)){
        /* a Markdown file: shown the way the browser shows one */
        u32 hl = 0;
        char *page = md_page(html, len, in, &hl);
        if(getenv("MD_HTML")) fwrite(page, 1, hl, stdout);
        html = page; len = hl;
    }

    if(getenv("PAGE_URL")) g_page_url = getenv("PAGE_URL");
    else {
        /* a mirror records the page's address next to it */
        char up[1100]; snprintf(up, sizeof up, "%s/url.txt", g_dir);
        u32 ul = 0; char *u = read_all(up, &ul);
        if(u){ while(ul && (u[ul-1] == '\n' || u[ul-1] == '\r')) u[--ul] = 0; g_page_url = u; }
    }
    web_set_url(g_page_url);
    web_set_viewport(width, 1000);
    web_set_adblock(getenv("NOADBLOCK") ? 0 : 1);
    { extern int js_debug_pos; js_debug_pos = getenv("JS_DEBUG") != NULL; }
    { const char *hm = getenv("JS_HEAP_MB"); js_set_heap_size((u32)(hm ? atoi(hm) : 256) << 20); }
    if(web_load(html, len) != 0){ fprintf(stderr, "web_load failed\n"); return 1; }

    /* external scripts, in document order */
    int nsc = web_script_count();
    for(int i = 0; i < nsc; i++){
        const char *src = web_script_src(i);
        if(!src) continue;
        char path[1200]; resolve(path, sizeof path, src);
        u32 slen = 0; char *js = read_all(path, &slen);
        if(js){ web_add_script(i, js, slen); fprintf(stderr, "script: %s (%u b)\n", path, slen); }
        else { web_add_script(i, "", 0); fprintf(stderr, "script MISSING: %s\n", g_last_abs); }
    }
    web_run_scripts();
    for(int i = 0; i < nsc; i++){
        const char *e = web_script_error(i);
        if(e && e[0]) fprintf(stderr, "script %d error: %s\n", i, e);
    }
    for(int t = 0; t < 50; t++) web_tick();
    if(getenv("CLICK")){
        /* click an element by id, like the OS does for a mouse click */
        char src[200]; snprintf(src, sizeof src, "this.__click = document.getElementById('%s');", getenv("CLICK"));
        js_run(src, (u32)strlen(src));
        js_val_t el = js_get(js_global(), "__click");
        if(el.t == JS_OBJ && js_host(el.o)){
            int ran = web_click(js_host(el.o));
            const void *box = web_activate(js_host(el.o));
            fprintf(stderr, "clicked #%s: %d handlers, opened%s\n", getenv("CLICK"), ran, box ? " (focus a text box)" : "");
            if(box && getenv("TYPE")){
                for(const char *q = getenv("TYPE"); *q; q++) web_key_input(box, *q);
                web_key_input(box, '\n');
                const char *nav = web_take_navigation();
                fprintf(stderr, "submitted: %s\n", nav ? nav : "(nothing)");
            }
        }
        else fprintf(stderr, "no element #%s\n", getenv("CLICK"));
        const char *e = web_js_error(); if(e && e[0]) fprintf(stderr, "after click: %s\n", e);
    }
    if(getenv("TICKS")){
        /* idle behaviour: how much script work the page does per tick after loading */
        extern int js_timers_tick(u64 now);
        int n = atoi(getenv("TICKS")), ran = 0;
        for(int t = 0; t < n; t++) ran += js_timers_tick(1000000ull + (u64)t);
        fprintf(stderr, "idle: %d timer callbacks in %d ticks\n", ran, n);
    }

    if(getenv("PROBE")){
        /* run timers for a while (simulated seconds), then evaluate an expression */
        extern int js_timers_tick(u64 now);
        int secs = getenv("PROBE_SECS") ? atoi(getenv("PROBE_SECS")) : 10;
        for(int t = 0; t < secs * 100; t++) js_timers_tick(2000000ull + (u64)t);
        char src[600]; snprintf(src, sizeof src, "this.__probe = String(%s);", getenv("PROBE"));
        js_run(src, (u32)strlen(src));
        js_val_t v = js_get(js_global(), "__probe");
        fprintf(stderr, "probe: %s\n", v.t == JS_STR ? js_to_str(v) : "(not a string)");
        const char *e = web_js_error(); if(e && e[0]) fprintf(stderr, "probe error: %s\n", e);
        const char *nav = web_take_navigation(); fprintf(stderr, "navigation: %s\n", nav ? nav : "(none)");
    }
    /* external stylesheets */
    int ns = web_stylesheet_count();
    for(int i = 0; i < ns; i++){
        const char *href = web_stylesheet(i);
        if(!href) continue;
        char path[1200]; resolve(path, sizeof path, href);
        u32 clen = 0; char *css = read_all(path, &clen);
        if(css){ web_add_css_at(i, css, clen); fprintf(stderr, "css: %s (%u b)\n", path, clen); }
        else fprintf(stderr, "css MISSING: %s\n", g_last_abs);
    }

    int page_h = web_layout(width);

    /* images */
    int ni = web_image_count();
    for(int i = 0; i < ni; i++){
        if(web_image_state(i) != 0 || !web_image_wanted(i)) continue;
        const char *href = web_image(i);
        if(!href) continue;
        if(strncmp(href, "data:", 5) == 0){ web_add_image_data_url(i, href); continue; }
        char path[1200]; resolve(path, sizeof path, href);
        u32 dlen = 0; char *data = read_all(path, &dlen);
        if(data){ web_add_image(i, data, dlen); fprintf(stderr, "img: %s (%u b)\n", path, dlen); }
        else { web_add_image(i, "", 0); fprintf(stderr, "img MISSING: %s\n", g_last_abs); }
    }
    page_h = web_layout(width);

    if(page_h < 1) page_h = 1;
    if(page_h > maxh) page_h = maxh;

    u32 *buf = malloc((size_t)width * page_h * 4);
    if(!buf){ fprintf(stderr, "oom frame %dx%d\n", width, page_h); return 1; }
    painter_target(buf, (u32)width, (u32)page_h);
    u32 canvas = 0xFFFFFF;                   /* the browser's default canvas */
    if(web_canvas_bg(&canvas)) fprintf(stderr, "canvas: #%06x\n", canvas);
    painter_clear(canvas);

    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    painter_cmds(cmds, n, 0, NULL);

    const char *jserr = web_js_error();
    if(jserr && jserr[0]) fprintf(stderr, "JS error: %s\n", jserr);
    const char *title = web_title();
    fprintf(stderr, "title: %s | %d cmds | %dx%d | %d sheets %d imgs\n",
            title ? title : "", n, width, page_h, ns, ni);

    FILE *f = fopen(out, "wb");
    if(!f){ fprintf(stderr, "cannot write %s\n", out); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", width, page_h);
    for(int y = 0; y < page_h; y++){
        for(int x = 0; x < width; x++){
            u32 c = buf[(size_t)y * width + x];
            unsigned char rgb[3] = { (c >> 16) & 255, (c >> 8) & 255, c & 255 };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}
