/* Drawing primitives, shared settings and helpers for the GUI and its apps. */
#include "amdroid/ui.h"
#include "amdroid/fb.h"
#include "amdroid/rtc.h"
#include "amdroid/timer.h"
#include "gui_internal.h"

/* ---- settings ---- */
static const u32 accent_colors[UI_NACCENT] = { 0x4CC2FF, 0x7C6CFF, 0x3DDC97, 0xFF8C42 };
static const char *accent_names[UI_NACCENT] = { "Blue", "Violet", "Green", "Orange" };
static int accent_idx = 0;

int ui_anim_on = 1;

int         ui_accent_index(void){ return accent_idx; }
void        ui_set_accent(int i){
    if(i < 0 || i >= UI_NACCENT || i == accent_idx) return;
    accent_idx = i;
    gui_damage_all();                       /* the colour is all over the desktop */
}
u32         ui_accent(void){ return accent_colors[accent_idx]; }
u32         ui_accent_color_of(int i){ return accent_colors[i]; }
const char *ui_accent_name(int i){ return accent_names[i]; }

/* ---- per-app accounting: CPU cycles spent in each app's code, and its heap blocks ---- */
#include "amdroid/heap.h"
#include "amdroid/sched.h"
static u64 app_cyc[UI_MAX_APPS];
static int cur_app = -1;
static u64 enter_tsc;
void ui_app_enter(int a){
    if(a < 0 || a >= UI_MAX_APPS) return;
    cur_app = a;
    heap_owner = a + 1;
    enter_tsc = sched_tsc();
}
void ui_app_leave(void){
    if(cur_app >= 0) app_cyc[cur_app] += sched_tsc() - enter_tsc;
    cur_app = -1;
    heap_owner = 0;
}
u64 ui_app_cycles(int a){ return a >= 0 && a < UI_MAX_APPS ? app_cyc[a] : 0; }

void ui_redraw(void){ win_mark_dirty(cur_app); }

void ui_msg_set(const char *s){ ui_notify(-1, s, ""); }
int ui_app_index(const struct app *app){
    for(int i = 0; i < UI_MAX_APPS && ui_apps[i]; i++) if(ui_apps[i] == app) return i;
    return -1;
}

/* ---- string helpers ---- */
void ui_num(u32 v, char *out){
    char tmp[12]; int n = 0;
    if(v == 0) tmp[n++] = '0';
    while(v){ tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int p = 0;
    for(int i = n - 1; i >= 0; i--) out[p++] = tmp[i];
    out[p] = 0;
}
void ui_cat(char *dst, const char *src){
    while(*dst) dst++;
    while((*dst++ = *src++)) ;
}
void ui_copy(char *dst, const char *src, u32 n){
    u32 i = 0;
    while(i + 1 < n && src[i]){ dst[i] = src[i]; i++; }
    dst[i] = 0;
}
int ui_streq(const char *a, const char *b){
    while(*a && *a == *b){ a++; b++; }
    return *a == *b;
}
u32 ui_len(const char *s){
    u32 n = 0;
    while(s[n]) n++;
    return n;
}
void ui_uptime(char *out, u64 ticks){
    u32 secs = (u32)(ticks / 100);
    u32 h = (secs / 3600) % 100, m = (secs / 60) % 60, s = secs % 60;
    out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10); out[2] = ':';
    out[3] = (char)('0' + m / 10); out[4] = (char)('0' + m % 10); out[5] = ':';
    out[6] = (char)('0' + s / 10); out[7] = (char)('0' + s % 10); out[8] = 0;
}
void ui_clock(char *out){
    rtc_time_t t;
    rtc_read(&t);
    out[0] = (char)('0' + t.hour / 10); out[1] = (char)('0' + t.hour % 10); out[2] = ':';
    out[3] = (char)('0' + t.min / 10);  out[4] = (char)('0' + t.min % 10);  out[5] = 0;
}
void ui_ip(char *out, u32 ip){
    char t[12];
    out[0] = 0;
    ui_num(ip >> 24, t);          ui_cat(out, t); ui_cat(out, ".");
    ui_num((ip >> 16) & 255, t);  ui_cat(out, t); ui_cat(out, ".");
    ui_num((ip >> 8) & 255, t);   ui_cat(out, t); ui_cat(out, ".");
    ui_num(ip & 255, t);          ui_cat(out, t);
}

/* ---- shapes ---- */
int ui_contains(rect_t r, int x, int y){
    return x >= (int)r.x && y >= (int)r.y && x < (int)(r.x + r.w) && y < (int)(r.y + r.h);
}
void ui_fill(rect_t r, u32 c){ fb_fill_rect(r.x, r.y, r.w, r.h, c); }

int ui_ease(int t){
    if(t <= 0) return 0;
    if(t >= 255) return 255;
    return 255 - (255 - t) * (255 - t) / 255;
}

u32 ui_lerp(u32 a, u32 b, int t){
    u32 out = 0;
    for(int sh = 16; sh >= 0; sh -= 8){
        int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        out |= (u32)(ca + (cb - ca) * t / 255) << sh;
    }
    return out;
}

static u32 isqrt(u32 v){
    u32 r = 0, bit = 1u << 30;
    while(bit > v) bit >>= 2;
    while(bit){
        if(v >= r + bit){ v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* Coverage of one corner of radius r (r x r bytes, 255 = inside), for the top left; the
 * other three are mirrors. Built once per radius and kept. */
#define RR_MAXR 32
static u8 *corner_cache[RR_MAXR + 1];
static const u8 *corner_mask(u32 r){
    if(corner_cache[r]) return corner_cache[r];
    int save = heap_owner; heap_owner = 0;
    u8 *m = kmalloc(r * r);
    heap_owner = save;
    if(!m) return 0;
    for(u32 py = 0; py < r; py++)
        for(u32 px = 0; px < r; px++){
            int dx = (int)(px * 256 + 128) - (int)(r * 256);
            int dy = (int)(py * 256 + 128) - (int)(r * 256);
            int cov = (int)(r * 256) - (int)isqrt((u32)(dx * dx + dy * dy)) + 128;   /* 1px-wide edge ramp */
            m[py * r + px] = (u8)(cov <= 0 ? 0 : cov >= 256 ? 255 : cov);
        }
    corner_cache[r] = m;
    return m;
}

/* Rounded rectangle with anti-aliased corners. The corner pixels blend over
 * whatever is already behind them, so the curve reads smooth, not stair-stepped. */
void ui_rrect(u32 x, u32 y, u32 w, u32 h, u32 r, u32 c){
    if(!fb_clip_hits((i32)x, (i32)y, (i32)w, (i32)h)) return;
    if(r > h / 2) r = h / 2;
    if(r > w / 2) r = w / 2;
    if(r > RR_MAXR) r = RR_MAXR;
    const u8 *m = r ? corner_mask(r) : 0;
    if(!m){ fb_fill_rect(x, y, w, h, c); return; }
    /* solid interior: middle band full width, then the straight top/bottom edges */
    fb_fill_rect(x,     y + r,         w,           h - 2 * r, c);
    fb_fill_rect(x + r, y,             w - 2 * r,   r,         c);
    fb_fill_rect(x + r, y + h - r,     w - 2 * r,   r,         c);
    /* the four corners, from the one mask, mirrored */
    u8 row[RR_MAXR];
    for(u32 py = 0; py < r; py++){
        const u8 *src = m + py * r;
        fb_blend_mask((i32)x, (i32)(y + py),         r, 1, src, c);
        fb_blend_mask((i32)x, (i32)(y + h - 1 - py), r, 1, src, c);
        for(u32 px = 0; px < r; px++) row[px] = src[r - 1 - px];
        fb_blend_mask((i32)(x + w - r), (i32)(y + py),         r, 1, row, c);
        fb_blend_mask((i32)(x + w - r), (i32)(y + h - 1 - py), r, 1, row, c);
    }
}

/* ---- anti-aliased round primitives (for icons) ----
 * Coverage comes from the pixel centre's distance to the shape edge, giving a
 * 1px soft edge blended over the background. */
void ui_disc(int cx, int cy, int r, u32 c){
    if(r <= 0 || !fb_clip_hits(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3)) return;
    int R = r * 256;
    for(int py = cy - r - 1; py <= cy + r + 1; py++)
        for(int px = cx - r - 1; px <= cx + r + 1; px++){
            int dx = (px * 256 + 128) - cx * 256;
            int dy = (py * 256 + 128) - cy * 256;
            u32 dist = isqrt((u32)(dx * dx + dy * dy));
            int cov = R - (int)dist + 128;
            if(cov <= 0) continue;
            if(cov > 256) cov = 256;
            if(px >= 0 && py >= 0) fb_blend((u32)px, (u32)py, c, (u32)cov);
        }
}

void ui_ring(int cx, int cy, int r, int thick, u32 c){
    if(r <= 0 || thick <= 0 || !fb_clip_hits(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3)) return;
    int Ro = r * 256, Ri = (r - thick) * 256;
    for(int py = cy - r - 1; py <= cy + r + 1; py++)
        for(int px = cx - r - 1; px <= cx + r + 1; px++){
            int dx = (px * 256 + 128) - cx * 256;
            int dy = (py * 256 + 128) - cy * 256;
            u32 dist = isqrt((u32)(dx * dx + dy * dy));
            int co = Ro - (int)dist + 128;      /* inside the outer edge */
            int ci = (int)dist - Ri + 128;      /* outside the inner edge */
            int cov = co < ci ? co : ci;
            if(cov <= 0) continue;
            if(cov > 256) cov = 256;
            if(px >= 0 && py >= 0) fb_blend((u32)px, (u32)py, c, (u32)cov);
        }
}

static u32 seg_dist(int px, int py, int ax, int ay, int bx, int by){  /* all 1/256 px */
    long abx = bx - ax, aby = by - ay;
    long apx = px - ax, apy = py - ay;
    long len2 = abx * abx + aby * aby;
    long t = len2 ? (apx * abx + apy * aby) : 0;
    if(t < 0) t = 0; else if(t > len2) t = len2;
    long qx = ax + (len2 ? abx * t / len2 : 0);
    long qy = ay + (len2 ? aby * t / len2 : 0);
    long dx = px - qx, dy = py - qy;
    return isqrt((u32)(dx * dx + dy * dy));
}

void ui_line(int x0, int y0, int x1, int y1, int thick, u32 c){
    if(thick <= 0) thick = 1;
    int ax = x0 * 256 + 128, ay = y0 * 256 + 128, bx = x1 * 256 + 128, by = y1 * 256 + 128;
    int half = thick * 128;                     /* half thickness, 1/256 px */
    int minx = (x0 < x1 ? x0 : x1) - thick - 1, maxx = (x0 > x1 ? x0 : x1) + thick + 1;
    int miny = (y0 < y1 ? y0 : y1) - thick - 1, maxy = (y0 > y1 ? y0 : y1) + thick + 1;
    if(!fb_clip_hits(minx, miny, maxx - minx + 1, maxy - miny + 1)) return;
    for(int py = miny; py <= maxy; py++)
        for(int px = minx; px <= maxx; px++){
            u32 dist = seg_dist(px * 256 + 128, py * 256 + 128, ax, ay, bx, by);
            int cov = half - (int)dist + 128;
            if(cov <= 0) continue;
            if(cov > 256) cov = 256;
            if(px >= 0 && py >= 0) fb_blend((u32)px, (u32)py, c, (u32)cov);
        }
}

/* Anti-aliased text. The 8x16 bitmap font is treated as a continuous field and
 * bilinearly resampled to the target size, so scaled-up glyphs get smooth edges
 * instead of chunky 2x2 blocks. Each covered pixel is alpha-blended with the
 * background. At scale 1 the sample points land exactly on source pixels, so
 * native-size text stays perfectly crisp. */
static int glyph_cov(const unsigned char *g, int fx, int fy){   /* fx,fy in 1/256 px; -> 0..256 */
    int x0 = fx >> 8, y0 = fy >> 8;
    int tx = fx & 255, ty = fy & 255;
    const unsigned char *r0 = (y0   >= 0 && y0   < FONT_H) ? &g[y0]   : 0;
    const unsigned char *r1 = (y0+1 >= 0 && y0+1 < FONT_H) ? &g[y0+1] : 0;
    int b00 = (r0 && x0  >= 0 && x0  < 8 && (*r0 & (0x80 >> x0)))     ? 256 : 0;
    int b10 = (r0 && x0+1 >= 0 && x0+1 < 8 && (*r0 & (0x80 >> (x0+1)))) ? 256 : 0;
    int b01 = (r1 && x0  >= 0 && x0  < 8 && (*r1 & (0x80 >> x0)))     ? 256 : 0;
    int b11 = (r1 && x0+1 >= 0 && x0+1 < 8 && (*r1 & (0x80 >> (x0+1)))) ? 256 : 0;
    int top = b00 + (b10 - b00) * tx / 256;
    int bot = b01 + (b11 - b01) * tx / 256;
    return top + (bot - top) * ty / 256;
}

/* Glyph cache: each (char, scale) is resampled to an 8-bit coverage mask once,
 * then reused every frame. The mask is colour- and background-independent, so
 * ui_text just alpha-blends it with the requested colour. This turns per-frame
 * text drawing from a resample-per-pixel into a single blend-per-pixel. */
#define GC_MAXSCALE 4
static unsigned char *glyph_cache[128][GC_MAXSCALE + 1];

static const unsigned char *glyph_mask(unsigned char c, int scale){
    if(c >= 128 || scale < 1 || scale > GC_MAXSCALE) return 0;
    if(glyph_cache[c][scale]) return glyph_cache[c][scale];
    int W = FONT_W * scale, H = FONT_H * scale;
    int save = heap_owner; heap_owner = 0;          /* cache is kernel-owned, not an app's */
    unsigned char *m = kmalloc((u32)(W * H));
    heap_owner = save;
    if(!m) return 0;
    const unsigned char *g = font8x16[c];
    for(int oy = 0; oy < H; oy++){
        int fy = ((oy * 2 + 1) << 7) / scale - 128;
        for(int ox = 0; ox < W; ox++){
            int fx = ((ox * 2 + 1) << 7) / scale - 128;
            int cov = glyph_cov(g, fx, fy);
            if(cov < 0) cov = 0; else if(cov > 255) cov = 255;
            m[oy * W + ox] = (unsigned char)cov;
        }
    }
    glyph_cache[c][scale] = m;
    return m;
}

void ui_text(u32 x, u32 y, const char *s, u32 fg, u32 scale){
    if(scale == 0) scale = 1;
    int W = FONT_W * (int)scale, H = FONT_H * (int)scale;
    if(!fb_clip_hits((i32)x, (i32)y, (i32)(ui_len(s) * (u32)W), H)) return;
    u32 cx = x;
    for(; *s; s++, cx += (u32)W){
        unsigned char c = (unsigned char)*s;
        if(c > 127) c = '?';
        if(c == ' ' || !fb_clip_hits((i32)cx, (i32)y, W, H)) continue;
        const unsigned char *m = glyph_mask(c, (int)scale);
        if(m){ fb_blend_mask((i32)cx, (i32)y, (u32)W, (u32)H, m, fg); continue; }
        /* a scale too big to cache: resample directly */
        const unsigned char *g = font8x16[c];
        for(int oy = 0; oy < H; oy++){
            int fy = ((oy * 2 + 1) << 7) / (int)scale - 128;
            for(int ox = 0; ox < W; ox++){
                int fx = ((ox * 2 + 1) << 7) / (int)scale - 128;
                int cov = glyph_cov(g, fx, fy);
                if(cov <= 0) continue;
                if(cov > 256) cov = 256;
                fb_blend(cx + (u32)ox, y + (u32)oy, fg, (u32)cov);
            }
        }
    }
}
u32 ui_text_w(const char *s, u32 scale){ return ui_len(s) * FONT_W * scale; }
void ui_text_right(rect_t r, u32 y, const char *s, u32 fg, u32 scale){
    ui_text(r.x + r.w - ui_text_w(s, scale), y, s, fg, scale);
}

/* ---- lists and buttons ---- */
rect_t ui_list_head(rect_t b){ return (rect_t){ b.x, b.y + UI_HEAD_Y, b.w, UI_ROW_H }; }
rect_t ui_list_row(rect_t b, int i){
    return (rect_t){ b.x, b.y + UI_HEAD_Y + UI_ROW_H + (u32)i * UI_ROW_H, b.w, UI_ROW_H };
}
int ui_list_rows(rect_t b){
    int n = ((int)b.h - UI_HEAD_Y - UI_ROW_H) / UI_ROW_H;
    return n < 1 ? 1 : n;
}
rect_t ui_button(rect_t b, u32 w){ return (rect_t){ b.x + b.w - w, b.y, w, UI_BTN_H }; }

/* ---- app registry access ---- */
int ui_app_count(void){
    int n = 0;
    while(n < UI_MAX_APPS && ui_apps[n]) n++;
    return n;
}
