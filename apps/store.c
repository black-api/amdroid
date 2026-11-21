/* App Store: browse the online catalog, install, update, open and remove apps.
 *
 * The catalog and the packages come from the store server (tools/store-server.py) over
 * HTTP; by default at 10.0.2.2:8642, which is the host in QEMU's user network. A file
 * STORE.CFG on the disk ("host" or "host:port") points it somewhere else. Downloads run
 * as a small state machine in the app's tick (non-blocking connect, then reads of what
 * has arrived), so the window keeps animating while the network works.
 *
 * Packages found on the disk (.adp files in its root) are listed in the Library and
 * installed through the same security check, with a confirmation sheet.
 *
 * Every rectangle the user can click is recorded while drawing (hit list), so clicks
 * always match what is on screen. Motion is time-based and eased; the window is redrawn
 * every tick only while something is moving. */
#include "amdroid/ui.h"
#include "amdroid/font.h"
#include "amdroid/pkg.h"
#include "amdroid/fuss.h"
#include "amdroid/keyboard.h"
#include "amdroid/mouse.h"
#include "amdroid/net.h"
#include "amdroid/timer.h"
#include "amdroid/fb.h"
#include "amdroid/fat32.h"

extern const app_t app_store;
extern void gui_damage_all(void);
extern void win_set_size(int a, int w, int h);
extern void win_move(int a, int x, int y);
extern void *kmalloc(unsigned long);
extern void kfree(void *);

/* ------------------------------------------------------------------ colours */
#define BG        C_WIN            /* the same as the window frame, so the body has no seam */
#define SURF      0x242E35
#define CARD      0x252F37
#define CARD_HI   0x303B44
#define LINE      0x303B43
#define TXT       C_TEXT
#define MUTE      C_MUTED
#define DIM       C_DIM

/* ------------------------------------------------------------------ small helpers */
static int imin(int a, int b){ return a < b ? a : b; }
static int imax(int a, int b){ return a > b ? a : b; }
static int clampi(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }
static u64 now_ms(void){ return timer_ticks() * 10; }

static u32 lighten(u32 c, int t){ return ui_lerp(c, 0xFFFFFF, t); }
static u32 darken(u32 c, int t){ return ui_lerp(c, 0x000000, t); }

/* t: 0..1000 */
static int ease_out(int t){                       /* cubic */
    if(t <= 0) return 0;
    if(t >= 1000) return 1000;
    i64 u = 1000 - t;
    return (int)(1000 - u * u * u / 1000000);
}
static int ease_back(int t){                      /* overshoots a little, then settles */
    if(t <= 0) return 0;
    if(t >= 1000) return 1000;
    i64 u = t - 1000;
    return (int)(1000 + 2702 * u * u * u / 1000000000 + 1702 * u * u / 1000000);
}
static int prog(u64 t0, u32 dur){                 /* 0..1000 of an animation that began at t0 */
    u64 n = now_ms();
    if(n <= t0) return 0;
    if(n - t0 >= dur) return 1000;
    return (int)((n - t0) * 1000 / dur);
}

static u32 slen(const char *s){ u32 n = 0; while(s[n]) n++; return n; }
static int seq(const char *a, const char *b){ while(*a && *a == *b){ a++; b++; } return *a == *b; }
static char low(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int has_ci(const char *hay, const char *needle){
    if(!*needle) return 1;
    for(; *hay; hay++){
        const char *a = hay, *b = needle;
        while(*a && *b && low(*a) == low(*b)){ a++; b++; }
        if(!*b) return 1;
    }
    return 0;
}

/* rounded rectangle that tolerates negative coordinates (scrolled off the top) */
static void rr(int x, int y, int w, int h, int r, u32 c){
    if(y < 0){ h += y; y = 0; }
    if(x < 0){ w += x; x = 0; }
    if(w <= 0 || h <= 0) return;
    ui_rrect((u32)x, (u32)y, (u32)w, (u32)h, (u32)r, c);
}
static void box(int x, int y, int w, int h, u32 c){
    if(y < 0){ h += y; y = 0; }
    if(x < 0){ w += x; x = 0; }
    if(w <= 0 || h <= 0) return;
    ui_fill((rect_t){ (u32)x, (u32)y, (u32)w, (u32)h }, c);
}
static void text(int x, int y, const char *s, u32 c, int scale){
    if(y < 0 || x < 0) return;
    ui_text((u32)x, (u32)y, s, c, (u32)scale);
}
static int tw(const char *s, int scale){ return (int)ui_text_w(s, (u32)scale); }

static u32 isq(u32 v){                            /* integer square root */
    u32 r = 0, bit = 1u << 30;
    while(bit > v) bit >>= 2;
    while(bit){
        if(v >= r + bit){ v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* a soft radial glow: n rings from radius r0 outward, 'step' px apart, strongest inside.
 * Drawn as horizontal spans between ring edges, so every pixel is written once. */
static void glow(int cx, int cy, int r0, int step, int n, u32 base, u32 col, int strength){
    int R = r0 + step * n;
    for(int dy = -R; dy <= R; dy++){
        int y = cy + dy;
        if(y < 0) continue;
        int ady = dy < 0 ? -dy : dy;
        int prev = 0;                                 /* half-width of the ring inside */
        if(r0 > ady) prev = (int)isq((u32)(r0 * r0 - ady * ady));
        for(int k = 0; k < n; k++){
            int rk = r0 + step * (k + 1);
            if(rk <= ady) continue;
            int hw = (int)isq((u32)(rk * rk - ady * ady));
            if(hw > prev){
                u32 c = ui_lerp(base, col, strength * (n - k) / n);
                box(cx - hw, y, hw - prev, 1, c);
                box(cx + prev, y, hw - prev, 1, c);
            }
            prev = hw;
        }
    }
}

/* a rounded rectangle filled with a left-to-right gradient */
static void grad_rr(int x, int y, int w, int h, int r, u32 c0, u32 c1){
    rr(x, y, w, h, r, ui_lerp(c0, c1, 128));          /* anti-aliased corners */
    if(r > h / 2) r = h / 2;
    for(int dx = 0; dx < w; dx += 2){
        int inset = 0;
        int ex = dx < r ? r - dx : (dx >= w - r ? dx - (w - r) + 1 : 0);
        if(ex > 0){ int e = r - (int)isq((u32)(r * r - (ex - 1) * (ex - 1))); inset = e + 1; }
        int t = w > 1 ? dx * 255 / (w - 1) : 0;
        box(x + dx, y + inset, imin(2, w - dx), h - 2 * inset, ui_lerp(c0, c1, t));
    }
}

/* copies s into out, cut with "..." so it fits in maxw pixels */
static void fit(char *out, u32 cap, const char *s, int maxw, int scale){
    u32 n = slen(s);
    int cw = FONT_W * scale;
    int fits = maxw / cw;
    if(fits < 0) fits = 0;
    if((int)n <= fits){ ui_copy(out, s, cap); return; }
    int keep = fits - 3; if(keep < 0) keep = 0;
    if((u32)keep > cap - 4) keep = (int)cap - 4;
    for(int i = 0; i < keep; i++) out[i] = s[i];
    out[keep] = '.'; out[keep + 1] = '.'; out[keep + 2] = '.'; out[keep + 3] = 0;
}

/* word-wrapped text; returns the y below it */
static int wrap(int x, int y, int maxw, const char *s, u32 c, int scale, int lh){
    int cw = FONT_W * scale, per = maxw / cw;
    if(per < 8) per = 8;
    char line[160];
    while(*s){
        while(*s == ' ') s++;
        int n = 0, last_sp = -1;
        while(s[n] && n < per && n < (int)sizeof line - 1){ if(s[n] == ' ') last_sp = n; n++; }
        if(s[n] && last_sp > 0) n = last_sp;
        for(int i = 0; i < n; i++) line[i] = s[i];
        line[n] = 0;
        text(x, y, line, c, scale);
        y += lh;
        s += n;
    }
    return y;
}

/* ------------------------------------------------------------------ the catalog */
typedef struct {
    char id[PKG_ID_MAX];
    char name[PKG_NAME_MAX];
    char version[16];
    char author[32];
    char tagline[72];
    char about[360];
    char category[16];
    char perms[64];
    char requires[16];
    u32 color;
    int featured;
    u32 size;
} capp_t;

#define CAT_MAX 32
static capp_t cat[CAT_MAX];
static int ncat;
static int cat_state;                 /* 0 loading, 1 loaded, 2 failed */
static u64 cat_t;                     /* when it changed */

static const char *const cats[] = { "All", "Games", "Creative", "Media", "Productivity", "Developer" };
#define NCATS 6

static u32 hexcol(const char *s){
    u32 v = 0;
    for(; *s; s++){
        char c = *s;
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if(d < 0) break;
        v = v * 16 + (u32)d;
    }
    return v & 0xFFFFFF;
}
static u32 num(const char *s){ u32 v = 0; while(*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0'); return v; }

static void parse_catalog(const char *d, u32 n){
    ncat = 0;
    if(n < 6 || d[0] != 'S' || d[1] != 'T') return;
    capp_t *a = 0;
    u32 i = 0;
    char key[16], val[360];
    while(i < n){
        u32 j = i;
        while(j < n && d[j] != '\n') j++;
        u32 len = j - i;
        if(len && d[i + len - 1] == '\r') len--;
        if(len >= 2 && d[i] == '@'){
            if(len == 4 && d[i+1] == 'E' && d[i+2] == 'N' && d[i+3] == 'D') break;
            if(ncat < CAT_MAX){
                a = &cat[ncat++];
                for(u32 k = 0; k < sizeof *a; k++) ((char *)a)[k] = 0;
                u32 m = len - 1; if(m > PKG_ID_MAX - 1) m = PKG_ID_MAX - 1;
                for(u32 k = 0; k < m; k++) a->id[k] = d[i + 1 + k];
                a->color = 0x5A8CF0;
            } else a = 0;
        } else if(a){
            u32 eq = 0;
            while(eq < len && d[i + eq] != '=') eq++;
            if(eq < len && eq < sizeof key){
                for(u32 k = 0; k < eq; k++) key[k] = d[i + k];
                key[eq] = 0;
                u32 vl = len - eq - 1; if(vl > sizeof val - 1) vl = sizeof val - 1;
                for(u32 k = 0; k < vl; k++) val[k] = d[i + eq + 1 + k];
                val[vl] = 0;
                if(seq(key, "name")) ui_copy(a->name, val, sizeof a->name);
                else if(seq(key, "version")) ui_copy(a->version, val, sizeof a->version);
                else if(seq(key, "author")) ui_copy(a->author, val, sizeof a->author);
                else if(seq(key, "tagline")) ui_copy(a->tagline, val, sizeof a->tagline);
                else if(seq(key, "about")) ui_copy(a->about, val, sizeof a->about);
                else if(seq(key, "category")) ui_copy(a->category, val, sizeof a->category);
                else if(seq(key, "perms")) ui_copy(a->perms, val, sizeof a->perms);
                else if(seq(key, "requires")) ui_copy(a->requires, val, sizeof a->requires);
                else if(seq(key, "color")) a->color = hexcol(val);
                else if(seq(key, "featured")) a->featured = val[0] == '1';
                else if(seq(key, "size")) a->size = num(val);
                else if(seq(key, "desc") && !a->tagline[0]) ui_copy(a->tagline, val, sizeof a->tagline);
            }
        }
        i = j + 1;
    }
    /* entries without a name are not shown */
    int k = 0;
    for(int m = 0; m < ncat; m++) if(cat[m].name[0]) cat[k++] = cat[m];
    ncat = k;
}

/* ------------------------------------------------------------------ installed apps */
static pkg_meta_t inst[PKG_MAX_APPS];
static int ninst;
static char disk_adp[PKG_MAX_APPS][16];
static int ndisk;

static void refresh_installed(void){
    ninst = pkg_list(inst, PKG_MAX_APPS);
    ndisk = pkg_available(disk_adp, PKG_MAX_APPS);
}
static int inst_of(const char *id){
    for(int i = 0; i < ninst; i++) if(seq(inst[i].id, id)) return i;
    return -1;
}
static int cat_of(const char *id){
    for(int i = 0; i < ncat; i++) if(seq(cat[i].id, id)) return i;
    return -1;
}
static int needs_update(int ci){
    int ii = inst_of(cat[ci].id);
    return ii >= 0 && !seq(inst[ii].version, cat[ci].version);
}
static int updates_count(void){
    int n = 0;
    for(int i = 0; i < ncat; i++) if(needs_update(i)) n++;
    return n;
}

/* ------------------------------------------------------------------ the store server */
static char srv_host[48] = "10.0.2.2";
static u16 srv_port = 8642;
static u32 srv_ip;

static int parse_ip(const char *s, u32 *ip){
    u32 v = 0;
    for(int part = 0; part < 4; part++){
        if(*s < '0' || *s > '9') return -1;
        u32 o = 0;
        while(*s >= '0' && *s <= '9') o = o * 10 + (u32)(*s++ - '0');
        if(o > 255) return -1;
        v = (v << 8) | o;
        if(part < 3){ if(*s != '.') return -1; s++; }
    }
    if(*s) return -1;
    *ip = v;
    return 0;
}

static void load_config(void){
    char buf[64];
    u32 got = 0;
    if(fat32_read_path("STORE.CFG", buf, sizeof buf - 1, &got) != 0 || !got) return;
    buf[got] = 0;
    u32 n = 0;
    while(buf[n] && buf[n] != '\n' && buf[n] != '\r' && buf[n] != ':' && n < sizeof srv_host - 1){ srv_host[n] = buf[n]; n++; }
    srv_host[n] = 0;
    if(buf[n] == ':') srv_port = (u16)num(buf + n + 1);
}

/* one HTTP GET at a time */
enum { J_IDLE, J_CONNECT, J_RECV, J_DONE, J_FAIL };
enum { K_CATALOG, K_PKG };
typedef struct {
    int state, kind, app;            /* app: catalog index for K_PKG */
    int sock;
    u8 *buf; u32 len, cap;
    u32 body, total;                 /* where the body starts, Content-Length (0 if unknown) */
    int status;
    u64 last;                        /* last progress, for the timeout */
    char path[48];
} job_t;
static job_t job;

#define QMAX 8
static int queue[QMAX], nqueue;      /* catalog indexes waiting to download */

static void job_free(void){
    if(job.sock >= 0){ tcp_close(job.sock); job.sock = -1; }
    if(job.buf){ kfree(job.buf); job.buf = 0; }
}

static int job_start(int kind, int app, const char *path){
    job_free();
    for(u32 k = 0; k < sizeof job; k++) ((char *)&job)[k] = 0;
    job.sock = -1;
    job.kind = kind; job.app = app;
    ui_copy(job.path, path, sizeof job.path);
    job.last = now_ms();
    if(!net_up()){ job.state = J_FAIL; return -1; }
    if(!srv_ip && parse_ip(srv_host, &srv_ip) != 0 && net_resolve(srv_host, &srv_ip, 0, 0) != 0){
        srv_ip = 0; job.state = J_FAIL; return -1;
    }
    job.cap = 16384;
    job.buf = kmalloc(job.cap);
    if(!job.buf){ job.state = J_FAIL; return -1; }
    job.sock = tcp_open(srv_ip, srv_port);
    job.state = job.sock >= 0 ? J_CONNECT : J_FAIL;
    return job.state == J_FAIL ? -1 : 0;
}

static void parse_head(void){
    for(u32 i = 0; i + 3 < job.len; i++){
        if(job.buf[i] == '\r' && job.buf[i+1] == '\n' && job.buf[i+2] == '\r' && job.buf[i+3] == '\n'){
            job.body = i + 4;
            job.status = (job.len > 12) ? (job.buf[9] - '0') * 100 + (job.buf[10] - '0') * 10 + (job.buf[11] - '0') : 0;
            /* Content-Length, for the progress bar */
            for(u32 k = 0; k + 15 < i; k++){
                const char *h = (const char *)job.buf + k;
                const char *want = "content-length:";
                u32 m = 0;
                while(m < 15 && low(h[m]) == want[m]) m++;
                if(m == 15){
                    u32 p = k + 15;
                    while(p < i && job.buf[p] == ' ') p++;
                    job.total = num((const char *)job.buf + p);
                    break;
                }
            }
            return;
        }
    }
}

/* advances the download; called every tick */
static void job_step(void){
    if(job.state == J_CONNECT){
        int r = tcp_open_poll(job.sock);
        if(r < 0){ job.sock = -1; job.state = J_FAIL; return; }
        if(r == 0) return;
        char req[160] = "GET ";
        ui_cat(req, job.path);
        ui_cat(req, " HTTP/1.0\r\nHost: ");
        ui_cat(req, srv_host);
        ui_cat(req, "\r\nUser-Agent: AmDroid-Store/1\r\n\r\n");
        if(tcp_send(job.sock, req, slen(req)) != 0){ job.state = J_FAIL; return; }
        job.state = J_RECV;
        job.last = now_ms();
        return;
    }
    if(job.state != J_RECV) return;
    net_poll();
    for(int rounds = 0; rounds < 32; rounds++){
        if(job.len + 4096 > job.cap){
            if(job.cap >= 2u * 1024 * 1024){ job.state = J_FAIL; return; }
            u8 *nb = kmalloc(job.cap * 2);
            if(!nb){ job.state = J_FAIL; return; }
            for(u32 k = 0; k < job.len; k++) nb[k] = job.buf[k];
            kfree(job.buf);
            job.buf = nb; job.cap *= 2;
        }
        int got = tcp_recv(job.sock, job.buf + job.len, job.cap - job.len - 1, 0);
        if(got <= 0) break;
        job.len += (u32)got;
        job.last = now_ms();
        if(!job.body) parse_head();
    }
    if(tcp_closed(job.sock) || (job.body && job.total && job.len - job.body >= job.total)){
        if(!job.body) parse_head();
        job.buf[job.len] = 0;
        job.state = (job.body && job.status == 200) ? J_DONE : J_FAIL;
        tcp_close(job.sock); job.sock = -1;
        return;
    }
    if(now_ms() - job.last > 8000){ job.state = J_FAIL; }
}

/* 0..1000 of the current download */
static int job_progress(void){
    if(job.state == J_CONNECT) return 30;
    if(job.state == J_DONE) return 1000;
    if(job.state != J_RECV) return 0;
    if(!job.body || !job.total) return 80;
    u32 got = job.len - job.body;
    return clampi((int)((u64)got * 1000 / job.total), 80, 1000);
}

/* ------------------------------------------------------------------ UI state */
enum { PH_SPLASH, PH_MAIN };
enum { PG_DISCOVER, PG_LIBRARY, PG_DETAIL };

static int phase;
static u64 open_t, main_t;            /* when the window opened; when the main view came in */
static int page, page_back;
static int tab_from;                  /* the tab the highlight glides from */
static u64 tab_t;
static u64 page_t;
static int detail;                    /* catalog index on the detail page */
static int chip;                      /* category filter */
static char search[28];
static int search_on;                 /* the search field has the keyboard */
static int scroll_cur, scroll_tgt, scroll_max;
static int banner, banner_prev;       /* featured index shown, and the one sliding out */
static u64 banner_t;
static int mx = -1, my = -1;          /* the pointer */
static rect_t body_last;

/* per catalog entry: hover glow 0..255, download progress shown (eased toward the real one) */
static int hov[CAT_MAX];
static int shown_prog[CAT_MAX];
static int busy[CAT_MAX];             /* 0 idle, 1 queued, 2 downloading, 3 installing, 4 just finished */
static u64 busy_t[CAT_MAX];

static char toast[80];
static u32 toast_col;
static u64 toast_t;
#define TOAST_MS 2600

/* the disk-package confirmation sheet (Library > On this disk) */
static int sheet;
static pkg_meta_t sh_meta;
static pkg_report_t sh_rep;
static char sh_file[16];
static u64 sheet_t;

static void say(const char *s, u32 c){ ui_copy(toast, s, sizeof toast); toast_col = c; toast_t = now_ms(); }

/* ------------------------------------------------------------------ hit list */
enum { H_NONE, H_TAB, H_CHIP, H_SEARCH, H_SEARCH_X, H_CARD, H_GET, H_BANNER, H_DOT, H_BACK,
       H_RETRY, H_LIB_OPEN, H_LIB_UPD, H_LIB_RM, H_LIB_ROW, H_DISK, H_SHEET_OK, H_SHEET_NO, H_SHEET };
typedef struct { rect_t r; int kind, arg; } hit_t;
#define HMAX 160
/* drawing fills 'building'; when the frame is done it becomes 'hits', which clicks and
 * hover read (so a button can ask whether it is hovered before it records itself) */
static hit_t hits[HMAX], building[HMAX];
static int nhits, nbuilding;
static void hit(int x, int y, int w, int h, int kind, int arg){
    if(y < 0){ h += y; y = 0; }
    if(nbuilding >= HMAX || w <= 0 || h <= 0) return;
    building[nbuilding++] = (hit_t){ { (u32)x, (u32)y, (u32)w, (u32)h }, kind, arg };
}
static void hits_commit(void){
    for(int i = 0; i < nbuilding; i++) hits[i] = building[i];
    nhits = nbuilding;
}
static const hit_t *hit_at(int x, int y){
    for(int i = nhits - 1; i >= 0; i--) if(ui_contains(hits[i].r, x, y)) return &hits[i];
    return 0;
}
static int hovered(int kind, int arg){
    if(mx < 0) return 0;
    const hit_t *h = hit_at(mx, my);
    return h && h->kind == kind && h->arg == arg;
}

/* ------------------------------------------------------------------ drawing pieces */

/* the app's tile: a gradient square with its initial */
static void tile(int x, int y, int size, u32 col, const char *name){
    int r = size / 4;
    grad_rr(x, y, size, size, r, lighten(col, 40), darken(col, 70));
    char ch[2] = { name[0] ? name[0] : '?', 0 };
    int sc = size >= 96 ? 5 : size >= 64 ? 3 : 2;
    int cw = FONT_W * sc, chh = FONT_H * sc;
    text(x + (size - cw) / 2 + 1, y + (size - chh) / 2 + 2, ch, darken(col, 120), sc);
    text(x + (size - cw) / 2, y + (size - chh) / 2, ch, 0xFFFFFF, sc);
}

static void pill(int x, int y, int w, int h, const char *label, u32 bg, u32 fg, int hov_on){
    u32 c = hov_on ? lighten(bg, 30) : bg;
    rr(x, y, w, h, h / 2, c);
    text(x + (w - tw(label, 1)) / 2, y + (h - FONT_H) / 2, label, fg, 1);
}

/* GET / OPEN / UPDATE, or the download progress, for catalog entry i */
static void action_button(int i, int x, int y, int w, int h, int kind){
    int hv = hovered(kind, i);
    if(busy[i] == 1 || busy[i] == 2 || busy[i] == 3){
        rr(x, y, w, h, h / 2, CARD_HI);
        int p = shown_prog[i];
        int fw = (w - 4) * p / 1000;
        if(fw > 0) rr(x + 2, y + 2, imax(fw, h - 4), h - 4, (h - 4) / 2, ui_accent());
        char lab[12];
        if(busy[i] == 1) ui_copy(lab, "Waiting", sizeof lab);
        else if(busy[i] == 3 || p >= 1000) ui_copy(lab, "Installing", sizeof lab);
        else { ui_num((u32)(p / 10), lab); ui_cat(lab, "%"); }
        int lx = x + (w - tw(lab, 1)) / 2, ly = y + (h - FONT_H) / 2;
        text(lx, ly, lab, (p > 500) ? 0x0A0E12 : TXT, 1);
        hit(x, y, w, h, kind, i);
        return;
    }
    int ii = inst_of(cat[i].id);
    if(ii >= 0 && needs_update(i)) pill(x, y, w, h, "UPDATE", C_WARN, 0x1A1205, hv);
    else if(ii >= 0) pill(x, y, w, h, "OPEN", ui_lerp(ui_accent(), 0x10161A, 175), lighten(ui_accent(), 40), hv);
    else pill(x, y, w, h, "GET", ui_accent(), 0x0A0E12, hv);
    hit(x, y, w, h, kind, i);
}

static void size_text(char *out, u32 sz){
    if(sz >= 1024){ ui_num((sz + 512) / 1024, out); ui_cat(out, " KB"); }
    else { ui_num(sz, out); ui_cat(out, " B"); }
}

/* a card in the grid */
#define CARD_H 176
static void card(int i, int x, int y, int w){
    int t = hov[i];
    u32 bg = ui_lerp(CARD, CARD_HI, t);
    int lift = t * 3 / 255;
    y -= lift;
    if(t) rr(x - 1, y - 1, w + 2, CARD_H + 2, 17, ui_lerp(CARD, cat[i].color, t / 5));
    rr(x, y, w, CARD_H, 16, bg);
    hit(x, y, w, CARD_H, H_CARD, i);              /* first: the button's hit goes on top of it */
    tile(x + 18, y + 18, 60, cat[i].color, cat[i].name);
    action_button(i, x + w - 18 - 92, y + 18 + 14, 92, 32, H_GET);
    char s[64];
    fit(s, sizeof s, cat[i].name, w - 36, 2);
    text(x + 18, y + 92, s, TXT, 2);
    fit(s, sizeof s, cat[i].tagline, w - 36, 1);
    text(x + 18, y + 128, s, MUTE, 1);
    char meta[48];
    ui_copy(meta, cat[i].category, sizeof meta);
    ui_cat(meta, "  -  ");
    char sz[16]; size_text(sz, cat[i].size); ui_cat(meta, sz);
    text(x + 18, y + 150, meta, DIM, 1);
}

/* a skeleton card, shimmering while the catalog loads */
static void skeleton(int x, int y, int w, int k){
    int ph = (int)((now_ms() / 4 + (u64)(k * 90)) % 500);
    int t = ph < 250 ? ph : 500 - ph;
    u32 hi = ui_lerp(CARD_HI, 0x36424A, t);
    rr(x, y, w, CARD_H, 16, CARD);
    rr(x + 18, y + 18, 60, 60, 15, hi);
    rr(x + w - 110, y + 32, 92, 32, 16, hi);
    rr(x + 18, y + 96, w * 5 / 10, 22, 8, hi);
    rr(x + 18, y + 130, w * 7 / 10, 12, 6, hi);
    rr(x + 18, y + 152, w * 3 / 10, 10, 5, hi);
}

/* the dots spinner */
static void spinner(int cx, int cy, int r, u32 col){
    static const int cs[12][2] = { {1000,0},{866,500},{500,866},{0,1000},{-500,866},{-866,500},
                                   {-1000,0},{-866,-500},{-500,-866},{0,-1000},{500,-866},{866,-500} };
    int head = (int)(now_ms() / 70 % 12);
    for(int k = 0; k < 12; k++){
        int age = (head - k + 12) % 12;
        int a = 255 - age * 20; if(a < 30) a = 30;
        ui_disc(cx + cs[k][0] * r / 1000, cy + cs[k][1] * r / 1000, age == 0 ? 4 : 3, ui_lerp(BG, col, a));
    }
}

/* ------------------------------------------------------------------ the splash */
static void draw_splash(rect_t b){
    box((int)b.x, (int)b.y, (int)b.w, (int)b.h, BG);
    int cx = (int)(b.x + b.w / 2), cy = (int)(b.y + b.h / 2) - 40;
    int el = (int)(now_ms() - open_t);
    /* leaving: everything rises and fades out */
    int out = 0;
    if(phase == PH_MAIN) out = ease_out(prog(main_t, 320));
    int rise = out * 40 / 1000;
    int fade = 255 - out * 255 / 1000;

    /* soft glow behind the logo */
    int g = ease_out(clampi(el * 1000 / 900, 0, 1000));
    glow(cx, cy - rise, 56, 1 + 7 * g / 1000, 18, BG, ui_accent(), 54 * fade / 255 * g / 1000);
    /* the logo pops in */
    int s = ease_back(clampi(el * 1000 / 600, 0, 1000));
    int size = 124 * s / 1000;
    if(size > 4){
        int x = cx - size / 2, y = cy - size / 2 - rise;
        grad_rr(x, y, size, size, size * 28 / 100, ui_lerp(BG, lighten(ui_accent(), 50), fade),
                ui_lerp(BG, darken(ui_accent(), 60), fade));
        if(size > 40) ui_icon_scaled(UI_ICO_STORE, (u32)(cx - size * 3 / 10), (u32)(y + size / 2 - size * 3 / 10),
                                     (u32)(size * 6 / 10), ui_lerp(BG, 0xFFFFFF, fade));
    }
    /* the title slides up and fades in */
    int tt = ease_out(clampi((el - 250) * 1000 / 500, 0, 1000));
    int ty = cy + 92 + (1000 - tt) * 18 / 1000 - rise;
    const char *title = "App Store";
    text(cx - tw(title, 3) / 2, ty, title, ui_lerp(BG, TXT, tt * 255 / 1000 * fade / 255), 3);
    int st = ease_out(clampi((el - 450) * 1000 / 500, 0, 1000));
    const char *sub = "Apps for Am-Droid";
    text(cx - tw(sub, 1) / 2, ty + 58, sub, ui_lerp(BG, MUTE, st * 255 / 1000 * fade / 255), 1);
    if(el > 700 && phase == PH_SPLASH){
        int sp = ease_out(clampi((el - 700) * 1000 / 300, 0, 1000));
        spinner(cx, ty + 120, 14, ui_lerp(BG, ui_accent(), sp * 255 / 1000));
    }
}

/* ------------------------------------------------------------------ top bar */
#define TOP_H 76
static void draw_top(rect_t b){
    int x = (int)b.x + 24, y = (int)b.y + 18;
    /* segmented tabs */
    const char *tabs[2] = { "Discover", "Library" };
    int tw0 = 128, th = 40;
    rr(x, y, tw0 * 2 + 8, th + 8, (th + 8) / 2, SURF);
    int sel = page == PG_LIBRARY ? 1 : 0;
    if(page == PG_DETAIL) sel = page_back == PG_LIBRARY ? 1 : 0;
    /* the highlight glides between the tabs */
    int pos = tab_from * tw0 + (sel - tab_from) * tw0 * ease_out(prog(tab_t, 280)) / 1000;
    rr(x + 4 + pos, y + 4, tw0, th, th / 2, CARD_HI);
    for(int i = 0; i < 2; i++){
        int lx = x + 4 + i * tw0;
        u32 c = i == sel ? TXT : (hovered(H_TAB, i) ? lighten(MUTE, 60) : MUTE);
        text(lx + (tw0 - tw(tabs[i], 1)) / 2, y + 4 + (th - FONT_H) / 2, tabs[i], c, 1);
        hit(lx, y + 4, tw0, th, H_TAB, i);
    }
    int nu = updates_count();
    if(nu){
        char n[6]; ui_num((u32)nu, n);
        int bx = x + 4 + 2 * tw0 - 18;
        ui_disc(bx, y + 8, 9, C_WARN);
        text(bx - tw(n, 1) / 2, y, n, 0x1A1205, 1);
    }
    /* search field */
    int sw = imin(320, (int)b.w - (tw0 * 2 + 8) - 90);
    int sx = (int)(b.x + b.w) - 24 - sw;
    if(sw > 120){
        u32 fb = search_on ? CARD_HI : (hovered(H_SEARCH, 0) ? lighten(SURF, 12) : SURF);
        if(search_on) rr(sx - 2, y - 2, sw + 4, th + 12, (th + 12) / 2, ui_accent());
        rr(sx, y, sw, th + 8, (th + 8) / 2, fb);
        ui_icon_scaled(UI_ICO_SEARCH, (u32)(sx + 14), (u32)(y + 12), 24, search_on ? ui_accent() : DIM);
        int tx = sx + 48;
        if(search[0]){
            char s[32]; fit(s, sizeof s, search, sw - 96, 1);
            text(tx, y + 16, s, TXT, 1);
            tx += tw(s, 1);
            /* the clear button */
            int cx = sx + sw - 24, cy = y + 24;
            ui_disc(cx, cy, 11, hovered(H_SEARCH_X, 0) ? TXT : MUTE);
            ui_line(cx - 4, cy - 4, cx + 4, cy + 4, 2, CARD_HI);
            ui_line(cx + 4, cy - 4, cx - 4, cy + 4, 2, CARD_HI);
            hit(cx - 14, cy - 14, 28, 28, H_SEARCH_X, 0);
        } else text(tx, y + 16, "Search apps", DIM, 1);
        if(search_on && (now_ms() / 530) % 2 == 0) box(tx + 1, y + 14, 2, 20, ui_accent());
        hit(sx, y, sw - (search[0] ? 40 : 0), th + 8, H_SEARCH, 0);
    }
    box((int)b.x, (int)b.y + TOP_H - 1, (int)b.w, 1, LINE);
}

/* ------------------------------------------------------------------ pages */
static int filtered(int i){
    if(chip && !seq(cat[i].category, cats[chip])) return 0;
    if(search[0] && !has_ci(cat[i].name, search) && !has_ci(cat[i].tagline, search) &&
       !has_ci(cat[i].category, search)) return 0;
    return 1;
}
static int nfeatured(void){ int n = 0; for(int i = 0; i < ncat; i++) n += cat[i].featured; return n; }
static int featured_at(int k){
    for(int i = 0; i < ncat; i++) if(cat[i].featured && k-- == 0) return i;
    return -1;
}

static void banner_card(int fi, int x, int y, int w, int h, int dx){
    int i = featured_at(fi);
    if(i < 0) return;
    u32 c = cat[i].color;
    grad_rr(x + dx, y, w, h, 22, darken(c, 175), darken(c, 95));
    tile(x + dx + w - 200, y + (h - 120) / 2, 120, c, cat[i].name);
    text(x + dx + 32, y + 28, "FEATURED", lighten(c, 80), 1);
    char s[64];
    fit(s, sizeof s, cat[i].name, w - 280, 3);
    text(x + dx + 32, y + 52, s, 0xFFFFFF, 3);
    fit(s, sizeof s, cat[i].tagline, w - 280, 1);
    text(x + dx + 32, y + 110, s, lighten(c, 160), 1);
    if(dx == 0){
        hit(x, y, w, h, H_BANNER, i);
        action_button(i, x + 32, y + h - 60, 112, 36, H_GET);   /* on top of the banner's hit */
    }
}

static int draw_discover(rect_t c, int y){
    int x = (int)c.x + 24, w = (int)c.w - 48;
    /* featured banner, sliding between apps */
    int nf = nfeatured();
    if(cat_state == 1 && nf && !search[0] && chip == 0){
        int h = 210;
        int p = ease_out(prog(banner_t, 520));
        fb_set_clip((u32)x, (u32)imax(y, (int)c.y), (u32)w, (u32)imax(0, h - imax(0, (int)c.y - y)));
        if(p < 1000 && banner_prev != banner){
            int off = (w + 24) * p / 1000;
            banner_card(banner_prev, x, y, w, h, -off);
            banner_card(banner, x, y, w, h, w + 24 - off);
        } else banner_card(banner, x, y, w, h, 0);
        fb_set_clip(c.x, c.y, c.w, c.h);
        /* dots */
        int dw = nf * 18;
        for(int k = 0; k < nf; k++){
            int dx = x + w - dw - 16 + k * 18 + 6, dy = y + h - 22;
            ui_disc(dx, dy, k == banner ? 5 : 4, k == banner ? 0xFFFFFF : 0x7F8D97);
            hit(dx - 8, dy - 8, 16, 16, H_DOT, k);
        }
        y += h + 28;
    }
    /* category chips */
    int cx = x;
    for(int k = 0; k < NCATS; k++){
        int cw = tw(cats[k], 1) + 32;
        if(cx + cw > x + w) break;
        int on = chip == k;
        u32 bg = on ? ui_accent() : (hovered(H_CHIP, k) ? CARD_HI : SURF);
        rr(cx, y, cw, 34, 17, bg);
        text(cx + 16, y + 9, cats[k], on ? 0x0A0E12 : TXT, 1);
        hit(cx, y, cw, 34, H_CHIP, k);
        cx += cw + 10;
    }
    y += 34 + 26;

    /* heading */
    char head[64];
    if(search[0]){ ui_copy(head, "Results for \"", sizeof head); ui_cat(head, search); ui_cat(head, "\""); }
    else ui_copy(head, chip ? cats[chip] : "All apps", sizeof head);
    text(x, y, head, TXT, 2);
    y += 48;

    int cols = imax(1, (w + 20) / (300 + 20));
    int cwid = (w - (cols - 1) * 20) / cols;
    if(cat_state == 0){
        for(int k = 0; k < cols * 2; k++)
            skeleton(x + (k % cols) * (cwid + 20), y + (k / cols) * (CARD_H + 20), cwid, k);
        return y + 2 * (CARD_H + 20);
    }
    if(cat_state == 2){
        int h = 200;
        rr(x, y, w, h, 18, SURF);
        ui_icon_scaled(UI_ICO_NET, (u32)(x + 32), (u32)(y + 34), 48, C_WARN);
        text(x + 104, y + 36, "Can't reach the store", TXT, 2);
        char where[96] = "Looked for the store server at ";
        ui_cat(where, srv_host); ui_cat(where, ":");
        char pn[8]; ui_num(srv_port, pn); ui_cat(where, pn);
        text(x + 104, y + 80, net_up() ? where : "There is no network connection.", MUTE, 1);
        text(x + 104, y + 102, "Start it on the host with  tools/store-server.py  and retry.", DIM, 1);
        text(x + 104, y + 124, "Apps already installed are in the Library.", DIM, 1);
        pill(x + 104, y + 148, 120, 36, "Try again", ui_accent(), 0x0A0E12, hovered(H_RETRY, 0));
        hit(x + 104, y + 148, 120, 36, H_RETRY, 0);
        return y + h + 24;
    }
    int k = 0;
    for(int i = 0; i < ncat; i++){
        if(!filtered(i)) continue;
        int cy = y + (k / cols) * (CARD_H + 20);
        if(cy + CARD_H > (int)c.y - 10 && cy < (int)(c.y + c.h) + 10)
            card(i, x + (k % cols) * (cwid + 20), cy, cwid);
        k++;
    }
    if(!k){
        text(x, y + 8, "No apps match.", MUTE, 2);
        text(x, y + 52, "Try another word or category.", DIM, 1);
        return y + 90;
    }
    return y + ((k + cols - 1) / cols) * (CARD_H + 20);
}

static int draw_detail(rect_t c, int y){
    int i = detail;
    if(i < 0 || i >= ncat) return y;
    capp_t *a = &cat[i];
    int x = (int)c.x + 32, w = (int)c.w - 64;
    /* back */
    pill(x - 8, y, 96, 34, "<  Back", hovered(H_BACK, 0) ? CARD_HI : SURF, TXT, 0);
    hit(x - 8, y, 96, 34, H_BACK, 0);
    y += 58;
    /* hero */
    tile(x, y, 128, a->color, a->name);
    char s[64];
    fit(s, sizeof s, a->name, w - 160, 3);
    text(x + 160, y + 4, s, TXT, 3);
    char by[64]; ui_copy(by, a->author[0] ? a->author : "Unknown", sizeof by);
    text(x + 160, y + 58, by, ui_accent(), 1);
    fit(s, sizeof s, a->tagline, w - 160, 1);
    text(x + 160, y + 80, s, MUTE, 1);
    action_button(i, x + 160, y + 104, 140, 40, H_GET);
    if(inst_of(a->id) >= 0 && !busy[i]){
        pill(x + 312, y + 104, 112, 40, "Remove", hovered(H_LIB_RM, 1000 + i) ? 0x4A2A30 : SURF, 0xFF8A8A, 0);
        hit(x + 312, y + 104, 112, 40, H_LIB_RM, 1000 + i);
    }
    y += 128 + 32;

    /* info strip */
    char ver[24] = "v"; ui_cat(ver, a->version);
    char sz[16]; size_text(sz, a->size);
    const char *lab[4] = { "VERSION", "SIZE", "CATEGORY", "NEEDS" };
    const char *val[4] = { ver, sz, a->category, a->requires[0] ? a->requires : "any" };
    int cw = w / 4;
    rr(x, y, w, 76, 16, SURF);
    for(int k = 0; k < 4; k++){
        int cx = x + k * cw;
        if(k) box(cx, y + 16, 1, 44, LINE);
        text(cx + (cw - tw(lab[k], 1)) / 2, y + 14, lab[k], DIM, 1);
        char v[24]; fit(v, sizeof v, val[k], cw - 16, 2);
        text(cx + (cw - tw(v, 2)) / 2, y + 34, v, TXT, 2);
    }
    y += 76 + 32;

    text(x, y, "About", TXT, 2);
    y += 44;
    y = wrap(x, y, imin(w, 760), a->about[0] ? a->about : a->tagline, MUTE, 1, 24);
    y += 24;

    text(x, y, "Permissions", TXT, 2);
    y += 44;
    const char *p = a->perms;
    if(!*p){ text(x, y, "None", MUTE, 1); y += 24; }
    while(*p){
        char one[24]; int n = 0;
        while(*p && *p != ',' && n < 23) one[n++] = *p++;
        one[n] = 0;
        if(*p == ',') p++;
        int danger = seq(one, "storage") || seq(one, "network") || seq(one, "system");
        ui_disc(x + 6, y + 8, 4, danger ? C_WARN : C_GOOD);
        const char *what = seq(one, "framebuffer") ? "Draws on the whole screen" :
                           seq(one, "input") ? "Reads the keyboard and mouse" :
                           seq(one, "storage") ? "Reads and writes files on the disk" :
                           seq(one, "network") ? "Uses the network" :
                           seq(one, "system") ? "Changes system settings" : "";
        text(x + 22, y, one, TXT, 1);
        text(x + 22 + 120, y, what, MUTE, 1);
        y += 26;
    }
    y += 16;
    rr(x, y, w, 56, 14, SURF);
    ui_disc(x + 24, y + 28, 8, C_GOOD);
    text(x + 44, y + 20, "Every package is checked before it installs: structure, platform version and a code scan.", MUTE, 1);
    return y + 56 + 24;
}

static int lib_row(int x, int y, int w, const char *name, u32 col, const char *sub, u32 subc){
    rr(x, y, w, 84, 16, CARD);
    tile(x + 16, y + 14, 56, col, name);
    char s[64]; fit(s, sizeof s, name, w - 380, 2);
    text(x + 88, y + 16, s, TXT, 2);
    fit(s, sizeof s, sub, w - 380, 1);
    text(x + 88, y + 52, s, subc, 1);
    return y;
}

static int draw_library(rect_t c, int y){
    int x = (int)c.x + 24, w = (int)c.w - 48;
    text(x, y, "Installed", TXT, 2);
    y += 48;
    if(!ninst){
        rr(x, y, w, 84, 16, SURF);
        text(x + 24, y + 22, "No apps installed yet.", MUTE, 1);
        text(x + 24, y + 46, "Find some in Discover.", DIM, 1);
        y += 84 + 14;
    }
    for(int k = 0; k < ninst; k++){
        int ci = cat_of(inst[k].id);
        u32 col = ci >= 0 ? cat[ci].color : 0x5A8CF0;
        char sub[64] = "v"; ui_cat(sub, inst[k].version);
        int upd = ci >= 0 && needs_update(ci);
        if(upd){ ui_cat(sub, "  -  update to v"); ui_cat(sub, cat[ci].version); }
        else if(inst[k].desc[0]){ ui_cat(sub, "  -  "); ui_cat(sub, inst[k].desc); }
        lib_row(x, y, w, inst[k].name, col, sub, upd ? C_WARN : MUTE);
        int bx = x + w - 16;
        bx -= 100;
        pill(bx, y + 24, 100, 36, "Remove", hovered(H_LIB_RM, k) ? 0x4A2A30 : SURF, 0xFF8A8A, 0);
        hit(bx, y + 24, 100, 36, H_LIB_RM, k);
        bx -= 112;
        if(upd){ action_button(ci, bx, y + 24, 100, 36, H_GET); bx -= 112; }
        pill(bx, y + 24, 100, 36, "Open", ui_accent(), 0x0A0E12, hovered(H_LIB_OPEN, k));
        hit(bx, y + 24, 100, 36, H_LIB_OPEN, k);
        hit(x, y, bx - x - 8, 84, H_LIB_ROW, k);
        y += 84 + 14;
    }
    y += 18;
    text(x, y, "On this disk", TXT, 2);
    y += 48;
    if(!ndisk){
        rr(x, y, w, 64, 16, SURF);
        text(x + 24, y + 24, "No .adp packages in the disk's root folder.", MUTE, 1);
        y += 64 + 14;
    }
    for(int k = 0; k < ndisk; k++){
        lib_row(x, y, w, disk_adp[k], 0x7F8D97, "A package file - checked before it installs", MUTE);
        pill(x + w - 116, y + 24, 100, 36, "Install", hovered(H_DISK, k) ? lighten(ui_accent(), 30) : ui_accent(), 0x0A0E12, 0);
        hit(x, y, w, 84, H_DISK, k);
        y += 84 + 14;
    }
    char src[96] = "Store server: ";
    ui_cat(src, srv_host); ui_cat(src, ":");
    char pn[8]; ui_num(srv_port, pn); ui_cat(src, pn);
    ui_cat(src, cat_state == 1 ? "  (connected)" : cat_state == 2 ? "  (unreachable)" : "  (connecting)");
    text(x, y + 12, src, DIM, 1);
    return y + 48;
}

/* the disk-package confirmation sheet */
static void draw_sheet(rect_t b){
    int p = ease_out(prog(sheet_t, 240));
    /* dim what is behind */
    fb_blend_rect(b.x, b.y, b.w, b.h, 0x05070A, (u32)(p * 170 / 1000));
    int w = imin(600, (int)b.w - 60), h = 360;
    int x = (int)b.x + ((int)b.w - w) / 2;
    int y = (int)b.y + ((int)b.h - h) / 2 + (1000 - p) * 40 / 1000;
    rr(x, y, w, h, 20, SURF);
    hit(x, y, w, h, H_SHEET, 0);
    int tx = x + 28, ty = y + 24;
    text(tx, ty, sh_rep.blocking ? "Install blocked" : "Install from disk?", sh_rep.blocking ? C_WARN : MUTE, 1);
    ty += 30;
    tile(tx, ty, 64, 0x7F8D97, sh_meta.name[0] ? sh_meta.name : sh_file);
    char s[64]; fit(s, sizeof s, sh_meta.name[0] ? sh_meta.name : sh_file, w - 140, 2);
    text(tx + 84, ty + 4, s, TXT, 2);
    char l[96] = "v"; ui_cat(l, sh_meta.version);
    if(sh_meta.author[0]){ ui_cat(l, "  -  "); ui_cat(l, sh_meta.author); }
    text(tx + 84, ty + 42, l, DIM, 1);
    ty += 88;
    char rq[48] = "Requires ";
    ui_cat(rq, sh_meta.requires[0] ? sh_meta.requires : "(unset)");
    text(tx, ty, rq, sh_rep.version_ok ? C_GOOD : C_WARN, 1); ty += 24;
    char pm[96] = "Permissions: "; ui_cat(pm, sh_meta.perms[0] ? sh_meta.perms : "none");
    text(tx, ty, pm, sh_rep.dangerous ? C_WARN : MUTE, 1); ty += 30;
    text(tx, ty, "Security scan", DIM, 1); ty += 22;
    if(sh_rep.error[0] && sh_rep.blocking) ty = wrap(tx, ty, w - 56, sh_rep.error, C_WARN, 1, 20);
    if(sh_rep.nfindings){
        /* findings are newline-separated */
        char f[96]; int n = 0;
        for(const char *q = sh_rep.findings; ; q++){
            if(*q == '\n' || !*q){ f[n] = 0; if(n){ text(tx, ty, f, C_WARN, 1); ty += 20; } n = 0; if(!*q) break; }
            else if(n < 95) f[n++] = *q;
            if(ty > y + h - 80) break;
        }
    } else if(!sh_rep.error[0]) text(tx, ty, "OK - no issues found", C_GOOD, 1);
    int by = y + h - 60;
    pill(x + w - 260, by, 112, 40, "Cancel", hovered(H_SHEET_NO, 0) ? CARD_HI : CARD, TXT, 0);
    hit(x + w - 260, by, 112, 40, H_SHEET_NO, 0);
    if(sh_rep.blocking) pill(x + w - 136, by, 112, 40, "Blocked", 0x3A2630, DIM, 0);
    else pill(x + w - 136, by, 112, 40, "Install", ui_accent(), 0x0A0E12, hovered(H_SHEET_OK, 0));
    hit(x + w - 136, by, 112, 40, H_SHEET_OK, 0);
}

static void draw_toast(rect_t b){
    if(!toast[0]) return;
    int el = (int)(now_ms() - toast_t);
    if(el > TOAST_MS){ toast[0] = 0; return; }
    int in = ease_back(clampi(el * 1000 / 320, 0, 1000));
    int out = el > TOAST_MS - 260 ? ease_out((el - (TOAST_MS - 260)) * 1000 / 260) : 0;
    int w = tw(toast, 1) + 64, h = 44;
    int x = (int)(b.x + (b.w - (u32)w) / 2);
    int y = (int)(b.y + b.h) - 24 - h + (1000 - in) * 60 / 1000 + out * 70 / 1000;
    rr(x, y, w, h, h / 2, 0x0B0F12);
    rr(x + 1, y + 1, w - 2, h - 2, h / 2 - 1, 0x232C33);
    ui_disc(x + 24, y + h / 2, 6, toast_col);
    text(x + 40, y + (h - FONT_H) / 2, toast, TXT, 1);
}

/* ------------------------------------------------------------------ the frame */
static int content_h;

static void s_draw(rect_t b, int focused){
    (void)focused;
    body_last = b;
    nbuilding = 0;
    if(phase == PH_SPLASH || prog(main_t, 320) < 1000){
        /* the splash finishes fading out before the store comes in */
        draw_splash(b);
        hits_commit();
        return;
    }
    box((int)b.x, (int)b.y, (int)b.w, (int)b.h, BG);

    /* the store slides in once; pages slide in when they change */
    int enter = ease_out(prog(main_t + 320, 480));
    int pg = ease_out(prog(page_t, 300));
    int off = (1000 - enter) * 50 / 1000 + (1000 - pg) * 28 / 1000;

    rect_t c = { b.x, b.y + TOP_H, b.w, b.h > TOP_H ? b.h - TOP_H : 0 };
    fb_set_clip(c.x, c.y, c.w, c.h);
    int y0 = (int)c.y + 24 - scroll_cur + off;
    int end;
    if(page == PG_DISCOVER) end = draw_discover(c, y0);
    else if(page == PG_DETAIL) end = draw_detail(c, y0);
    else end = draw_library(c, y0);
    content_h = end - y0 + 24;
    scroll_max = imax(0, content_h - (int)c.h);
    /* scrollbar */
    if(scroll_max > 0){
        int th = imax(40, (int)c.h * (int)c.h / content_h);
        int ty = (int)c.y + ((int)c.h - th) * clampi(scroll_cur, 0, scroll_max) / scroll_max;
        rr((int)(c.x + c.w) - 8, ty + 4, 4, th - 8, 2, LINE);
    }
    /* a page that just changed fades in from the background */
    if(pg < 1000) fb_blend_rect(c.x, c.y, c.w, c.h, BG, (u32)((1000 - pg) * 230 / 1000));
    fb_set_clip(b.x, b.y, b.w, b.h);
    draw_top(b);
    /* the whole store fades in after the splash */
    if(enter < 1000) fb_blend_rect(b.x, b.y, b.w, b.h, BG, (u32)((1000 - enter) * 256 / 1000));
    draw_toast(b);
    if(sheet) draw_sheet(b);
    hits_commit();
}

/* ------------------------------------------------------------------ actions */
static int tab_of(int pg, int back){ return (pg == PG_LIBRARY || (pg == PG_DETAIL && back == PG_LIBRARY)) ? 1 : 0; }
static void go(int pg){
    if(pg == page) return;
    int was = tab_of(page, page_back);
    if(pg == PG_DETAIL) page_back = page;
    page = pg;
    page_t = now_ms();
    scroll_cur = scroll_tgt = 0;
    if(pg == PG_LIBRARY) refresh_installed();
    if(tab_of(page, page_back) != was){ tab_from = was; tab_t = now_ms(); }
}

static void start_catalog(void){
    cat_state = 0;
    cat_t = now_ms();
    if(job_start(K_CATALOG, -1, "/catalog") != 0){ cat_state = 2; cat_t = now_ms(); }
}

static void next_download(void){
    if(job.state != J_IDLE || !nqueue) return;      /* one download at a time */
    int i = queue[0];
    for(int k = 1; k < nqueue; k++) queue[k - 1] = queue[k];
    nqueue--;
    char path[40] = "/pkg/";
    ui_cat(path, cat[i].id);
    busy[i] = 2; busy_t[i] = now_ms(); shown_prog[i] = 0;
    if(job_start(K_PKG, i, path) != 0){
        busy[i] = 0;
        say("Download failed - the store is unreachable", C_WARN);
    }
}

static void get_app(int i){
    if(busy[i] == 1 || busy[i] == 2 || busy[i] == 3) return;
    if(!fat32_ready()){ say("No disk to install apps to - attach a FAT32 disk", C_WARN); return; }
    if(nqueue >= QMAX) return;
    busy[i] = 1; busy_t[i] = now_ms(); shown_prog[i] = 0;
    queue[nqueue++] = i;
    next_download();
}

static void launch(const char *id){
    char path[64];
    if(pkg_entry_path(id, path, sizeof path) != 0){ say("Cannot find the app's program", C_WARN); return; }
    mouse_event_t ev;
    while(mouse_poll(&ev)) { }
    fuss_run_file(path);
    while(mouse_poll(&ev)) { }
    gui_damage_all();
}

static void remove_app(const char *id, const char *name){
    char msg[80];
    if(pkg_remove(id) == 0){ ui_copy(msg, name, sizeof msg); ui_cat(msg, " removed"); say(msg, MUTE); }
    else say("Could not remove the app", C_WARN);
    refresh_installed();
}

static void primary(int i){
    int ii = inst_of(cat[i].id);
    if(ii >= 0 && !needs_update(i)) launch(cat[i].id);
    else get_app(i);
}

/* a finished download: check, install, report */
static void finish_job(void){
    if(job.kind == K_CATALOG){
        if(job.state == J_DONE){
            parse_catalog((const char *)job.buf + job.body, job.len - job.body);
            cat_state = ncat ? 1 : 2;
        } else cat_state = 2;
        cat_t = now_ms();
        banner = banner_prev = 0;
        banner_t = now_ms();
        job_free();
        job.state = J_IDLE;
        return;
    }
    int i = job.app;
    if(job.state != J_DONE){
        busy[i] = 0;
        say("Download failed", C_WARN);
        job_free(); job.state = J_IDLE;
        return;
    }
    pkg_meta_t m; pkg_report_t rep;
    const char *data = (const char *)job.buf + job.body;
    u32 n = job.len - job.body;
    char msg[80];
    if(pkg_check(data, n, &m, &rep) != 0 && rep.blocking){
        ui_copy(msg, "Blocked: ", sizeof msg);
        ui_cat(msg, rep.error[0] ? rep.error : "failed the security scan");
        say(msg, C_WARN);
        busy[i] = 0;
    } else {
        char err[104] = "";
        pkg_meta_t out;
        int was = inst_of(cat[i].id) >= 0;
        if(pkg_install(data, n, &out, err, sizeof err) == 0){
            ui_copy(msg, out.name, sizeof msg);
            ui_cat(msg, was ? " updated" : " installed");
            say(msg, C_GOOD);
            ui_notify(ui_app_index(&app_store), was ? "App updated" : "App installed", out.name);
        } else {
            ui_copy(msg, "Install failed: ", sizeof msg);
            ui_cat(msg, err[0] ? err : "unknown error");
            say(msg, C_WARN);
        }
        busy[i] = 4; busy_t[i] = now_ms();
        refresh_installed();
    }
    job_free();
    job.state = J_IDLE;
}

static void open_disk_sheet(const char *file){
    u32 sz = 0;
    if(fat32_read_path(file, 0, 0, &sz) != 0 || !sz) return;
    char *buf = kmalloc(sz + 1);
    if(!buf) return;
    u32 got = 0;
    if(fat32_read_path(file, buf, sz, &got) == 0){
        buf[got] = 0;
        pkg_check(buf, got, &sh_meta, &sh_rep);
        ui_copy(sh_file, file, sizeof sh_file);
        sheet = 1; sheet_t = now_ms();
    }
    kfree(buf);
}

static void sheet_install(void){
    if(sh_rep.blocking){ say("Blocked by the security scan", C_WARN); sheet = 0; return; }
    pkg_meta_t out; char err[104] = "";
    char msg[80];
    if(pkg_install_file(sh_file, &out, err, sizeof err) == 0){
        ui_copy(msg, out.name, sizeof msg); ui_cat(msg, " installed");
        say(msg, C_GOOD);
        ui_notify(ui_app_index(&app_store), "App installed", out.name);
    } else {
        ui_copy(msg, "Install failed: ", sizeof msg); ui_cat(msg, err[0] ? err : "unknown error");
        say(msg, C_WARN);
    }
    sheet = 0;
    refresh_installed();
}

/* ------------------------------------------------------------------ app callbacks */
static void s_open(void){
    static int sized;
    if(!sized){
        /* the store wants room for three columns of cards */
        int a = ui_app_index(&app_store);
        int sw = (int)fb_width(), sh = (int)fb_height();
        int w = imin(1180, sw - 80), h = imin(800, sh - 150);
        if(a >= 0 && w > 700 && h > 500){
            win_set_size(a, w, h);
            win_move(a, (sw - w) / 2, imax(72, (sh - 60 - h) / 2 + 10));
        }
        sized = 1;
    }
    phase = PH_SPLASH;
    open_t = now_ms();
    main_t = 0;
    page = PG_DISCOVER; page_back = PG_DISCOVER; page_t = 0;
    tab_from = 0; tab_t = 0;
    chip = 0; search[0] = 0; search_on = 0;
    scroll_cur = scroll_tgt = 0;
    sheet = 0; toast[0] = 0;
    nqueue = 0;
    for(int i = 0; i < CAT_MAX; i++){ hov[i] = 0; busy[i] = 0; shown_prog[i] = 0; }
    srv_ip = 0;
    load_config();
    refresh_installed();
    start_catalog();
}

static void s_close(void){
    job_free();
    job.state = J_IDLE;
    nqueue = 0;
}

static void s_tick(void){
    int anim = 0;
    u64 n = now_ms();

    /* network */
    if(job.state == J_CONNECT || job.state == J_RECV){ job_step(); anim = 1; }
    if(job.state == J_DONE || job.state == J_FAIL){
        if(job.kind == K_PKG && job.state == J_DONE && shown_prog[job.app] < 1000){ /* let the bar finish */ }
        else { finish_job(); next_download(); }
        anim = 1;
    }
    if(job.state == J_IDLE && nqueue) next_download();

    /* splash -> main once the logo has played and the catalog answered (or 2.4 s passed) */
    if(phase == PH_SPLASH){
        anim = 1;
        u64 el = n - open_t;
        if(el > 1500 && (cat_state != 0 || el > 2400)){ phase = PH_MAIN; main_t = n; }
    } else if(n - main_t < 900) anim = 1;

    /* progress bars ease toward the real progress (and take at least ~0.6 s) */
    for(int i = 0; i < ncat; i++){
        if(busy[i] == 2 || busy[i] == 3){
            int real = (job.kind == K_PKG && job.app == i) ? job_progress() : 0;
            int cap = (int)((n - busy_t[i]) * 1000 / 650);
            int tgt = imin(real, cap);
            if(shown_prog[i] < tgt) shown_prog[i] += imax(1, (tgt - shown_prog[i]) / 5);
            anim = 1;
        } else if(busy[i] == 1) anim = 1;
        else if(busy[i] == 4 && n - busy_t[i] > 200){ busy[i] = 0; anim = 1; }
    }

    /* hover glow */
    int hc = -1;
    if(mx >= 0 && !sheet){ const hit_t *h = hit_at(mx, my); if(h && (h->kind == H_CARD || h->kind == H_GET)) hc = h->arg; }
    for(int i = 0; i < ncat; i++){
        int tgt = i == hc ? 255 : 0;
        if(hov[i] != tgt){ hov[i] += (tgt - hov[i]) / 3 + (tgt > hov[i] ? 1 : -1); hov[i] = clampi(hov[i], 0, 255); anim = 1; }
    }

    /* smooth scrolling */
    scroll_tgt = clampi(scroll_tgt, 0, scroll_max);
    if(scroll_cur != scroll_tgt){
        int d = scroll_tgt - scroll_cur;
        scroll_cur += d / 4 + (d > 0 ? 1 : -1);
        if((d > 0 && scroll_cur > scroll_tgt) || (d < 0 && scroll_cur < scroll_tgt)) scroll_cur = scroll_tgt;
        anim = 1;
    }

    /* the featured banner moves on every 6 s, unless the pointer rests on it */
    int nf = nfeatured();
    if(phase == PH_MAIN && page == PG_DISCOVER && cat_state == 1 && nf > 1 && !search[0] && !chip){
        int on_banner = mx >= 0 && hovered(H_BANNER, featured_at(banner));
        if(n - banner_t > 6000 && !on_banner && !sheet){
            banner_prev = banner; banner = (banner + 1) % nf; banner_t = n;
        }
        if(n - banner_t < 560) anim = 1;
    }
    if(n - page_t < 320) anim = 1;
    if(cat_state == 0 && phase == PH_MAIN) anim = 1;          /* skeleton shimmer */
    if(toast[0]) anim = 1;
    if(sheet && n - sheet_t < 260) anim = 1;
    if(search_on){ static u64 blink; if(n / 530 != blink){ blink = n / 530; anim = 1; } }
    if(anim) ui_redraw();
}

static void s_click(rect_t b, int x, int y){
    (void)b;
    if(phase == PH_SPLASH) return;
    const hit_t *h = hit_at(x, y);
    if(sheet){
        if(!h || h->kind == H_SHEET_NO){ sheet = 0; return; }
        if(h->kind == H_SHEET_OK) sheet_install();
        return;
    }
    if(!h){ search_on = 0; return; }
    int k = h->kind, a = h->arg;
    if(k != H_SEARCH) search_on = 0;
    switch(k){
        case H_TAB: go(a ? PG_LIBRARY : PG_DISCOVER); break;
        case H_CHIP: if(chip != a){ chip = a; page_t = now_ms(); scroll_tgt = 0; } break;
        case H_SEARCH: search_on = 1; if(page != PG_DISCOVER) go(PG_DISCOVER); break;
        case H_SEARCH_X: search[0] = 0; search_on = 1; page_t = now_ms(); break;
        case H_CARD: case H_BANNER: detail = a; go(PG_DETAIL); break;
        case H_GET: primary(a); break;
        case H_DOT: if(a != banner){ banner_prev = banner; banner = a; banner_t = now_ms(); } break;
        case H_BACK: go(page_back); break;
        case H_RETRY: start_catalog(); break;
        case H_LIB_OPEN: launch(inst[a].id); break;
        case H_LIB_RM:
            if(a >= 1000){ int ci = a - 1000; remove_app(cat[ci].id, cat[ci].name); }
            else if(a < ninst){ char id[PKG_ID_MAX], nm[PKG_NAME_MAX]; ui_copy(id, inst[a].id, sizeof id); ui_copy(nm, inst[a].name, sizeof nm); remove_app(id, nm); }
            break;
        case H_LIB_ROW: { int ci = cat_of(inst[a].id); if(ci >= 0){ detail = ci; go(PG_DETAIL); } } break;
        case H_DISK: open_disk_sheet(disk_adp[a]); break;
    }
}

static void s_move(rect_t b, int x, int y){
    (void)b;
    int was_kind = -1, was_arg = -1, now_kind = -1, now_arg = -1;
    if(mx >= 0){ const hit_t *h = hit_at(mx, my); if(h){ was_kind = h->kind; was_arg = h->arg; } }
    mx = x; my = y;
    if(mx >= 0){ const hit_t *h = hit_at(mx, my); if(h){ now_kind = h->kind; now_arg = h->arg; } }
    if(was_kind != now_kind || was_arg != now_arg) ui_redraw();
}

static int s_key(int c){
    if(phase == PH_SPLASH) return c != 27 ? 1 : 0;
    if(sheet){
        if(c == 27) sheet = 0;
        else if(c == '\n' || c == '\r') sheet_install();
        return 1;
    }
    if(c == 27){
        if(search_on && search[0]){ search[0] = 0; page_t = now_ms(); return 1; }
        if(search_on){ search_on = 0; return 1; }
        if(page == PG_DETAIL){ go(page_back); return 1; }
        return 0;                                    /* closes the window */
    }
    if(c == KEY_UP){ scroll_tgt -= 80; return 1; }
    if(c == KEY_DOWN){ scroll_tgt += 80; return 1; }
    if(c == KEY_PGUP){ scroll_tgt -= (int)body_last.h - 160; return 1; }
    if(c == KEY_PGDN){ scroll_tgt += (int)body_last.h - 160; return 1; }
    if(c == KEY_HOME){ scroll_tgt = 0; return 1; }
    if(c == KEY_END){ scroll_tgt = scroll_max; return 1; }
    if(c == '\t'){ go(page == PG_LIBRARY ? PG_DISCOVER : PG_LIBRARY); return 1; }
    if(c == 8 || c == 0x7F){
        if(search_on || search[0]){ u32 n = slen(search); if(n) search[n - 1] = 0; page_t = now_ms(); scroll_tgt = 0; }
        return 1;
    }
    if((c == '\n' || c == '\r') && page == PG_DETAIL){ primary(detail); return 1; }
    if(c >= 32 && c < 127 && page != PG_LIBRARY){
        if(page == PG_DETAIL) go(PG_DISCOVER);
        search_on = 1;
        u32 n = slen(search);
        if(n < sizeof search - 1){ search[n] = (char)c; search[n + 1] = 0; }
        page_t = now_ms(); scroll_tgt = 0;
        return 1;
    }
    return 1;
}

static void s_scroll(rect_t b, int x, int y, int dz){
    (void)b; (void)x; (void)y;
    if(phase == PH_SPLASH || sheet) return;
    scroll_tgt = clampi(scroll_tgt + dz * 90, 0, scroll_max);
}

const app_t app_store = {
    .name = "App Store",
    .icon = UI_ICO_STORE,
    .open = s_open,
    .close = s_close,
    .draw = s_draw,
    .key = s_key,
    .click = s_click,
    .move = s_move,
    .tick = s_tick,
    .scroll = s_scroll,
};
