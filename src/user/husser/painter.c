/* painter.c — draws the engine's command list into the viewport buffer that
 * the shared region exposes. The page is painted by Husser, not by the GUI:
 * the browser only blits the result. Same anti-aliased text as the desktop. */
#include "amdroid/husser_proto.h"
#include "amdroid/web.h"
#include "amdroid/font.h"
#include "string.h"

static u32 *vram;                 /* where the frame is painted (set by husser.c) */
static u32 vp_w, vp_h;
#define VP_PITCH (vp_w)

static void pxfill(u32 x, u32 y, u32 w, u32 h, u32 c){
    if(!vp_w) return;
    u32 x1 = x + w, y1 = y + h;
    if(x >= vp_w || y >= vp_h) return;
    if(x1 > vp_w) x1 = vp_w;
    if(y1 > vp_h) y1 = vp_h;
    for(u32 yy = y; yy < y1; yy++){
        u32 *row = (u32 *)((u8 *)vram + (u64)yy * VP_PITCH * 4) + x;
        for(u32 xx = x; xx < x1; xx++) *row++ = c;
    }
}
static void blendpx(u32 x, u32 y, u32 c, u32 a){
    if(x >= vp_w || y >= vp_h || !a) return;
    u32 *p = &vram[y * VP_PITCH + x];
    if(a >= 256){ *p = c; return; }
    u32 bg = *p, ia = 256 - a;
    u32 rb = ((((bg & 0x00FF00FF) * ia + (c & 0x00FF00FF) * a) >> 8) & 0x00FF00FF);
    u32 g  = ((((bg & 0x0000FF00) * ia + (c & 0x0000FF00) * a) >> 8) & 0x0000FF00);
    *p = rb | g;
}

void painter_target(u32 *buf, u32 w, u32 h){ vram = buf; vp_w = w; vp_h = h; }

void painter_clear(u32 c){ pxfill(0, 0, vp_w, vp_h, c); }

/* ---- anti-aliased glyph coverage (same field resample as the desktop) ---- */
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
/* Each glyph is resampled once per size into a coverage mask and kept: painting text is
 * then one blend a pixel, and whole runs of clear or solid pixels go eight at a time. */
#define GC_MAXSCALE 4
static unsigned char gc_mask[128][GC_MAXSCALE][FONT_W * GC_MAXSCALE * FONT_H * GC_MAXSCALE];
static unsigned char gc_have[128][GC_MAXSCALE];

static const unsigned char *glyph_mask(unsigned char c, int scale){
    unsigned char *m = gc_mask[c][scale - 1];
    if(gc_have[c][scale - 1]) return m;
    int W = FONT_W * scale, H = FONT_H * scale;
    const unsigned char *g = font8x16[c];
    for(int oy = 0; oy < H; oy++){
        int fy = ((oy * 2 + 1) << 7) / scale - 128;
        for(int ox = 0; ox < W; ox++){
            int cov = glyph_cov(g, ((ox * 2 + 1) << 7) / scale - 128, fy);
            m[oy * W + ox] = (unsigned char)(cov < 0 ? 0 : cov > 255 ? 255 : cov);
        }
    }
    gc_have[c][scale - 1] = 1;
    return m;
}

static void draw_glyph(int x, int y, const unsigned char *m, int W, int H, u32 color){
    int x0 = x < 0 ? 0 : x, x1 = x + W > (int)vp_w ? (int)vp_w : x + W;
    int y0 = y < 0 ? 0 : y, y1 = y + H > (int)vp_h ? (int)vp_h : y + H;
    if(x1 <= x0) return;
    for(int yy = y0; yy < y1; yy++){
        const unsigned char *s = m + (yy - y) * W + (x0 - x);
        int xx = x0;
        for(; xx + 8 <= x1; xx += 8, s += 8){
            u64 v = *(const u64 *)s;
            if(v == 0) continue;
            if(v == ~0ULL){ pxfill((u32)xx, (u32)yy, 8, 1, color); continue; }
            for(int k = 0; k < 8; k++) blendpx((u32)(xx + k), (u32)yy, color, s[k] == 255 ? 256 : s[k]);
        }
        for(; xx < x1; xx++, s++) blendpx((u32)xx, (u32)yy, color, *s == 255 ? 256 : *s);
    }
}

/* A glyph sheared to the right by (H-1-row)/4 px, for italics. */
static void draw_glyph_italic(int x, int y, const unsigned char *m, int W, int H, u32 color){
    for(int ry = 0; ry < H; ry++){
        int sh = (H - 1 - ry) >> 2;
        const unsigned char *s = m + ry * W;
        for(int cx = 0; cx < W; cx++){
            unsigned char cov = s[cx];
            if(cov) blendpx((u32)(x + cx + sh), (u32)(y + ry), color, cov == 255 ? 256 : cov);
        }
    }
}

static void draw_text(int x, int y, const char *s, u32 color, u32 scale, int italic){
    if(!scale) scale = 1;
    int W = FONT_W * (int)scale, H = FONT_H * (int)scale;
    if(y + H <= 0 || y >= (int)vp_h) return;
    for(; *s; s++, x += W){
        unsigned char c = (unsigned char)*s;
        if(c > 127) c = '?';
        if(c == ' ' || x + W <= 0) continue;
        if(x >= (int)vp_w) break;
        if(scale <= GC_MAXSCALE){
            if(italic) draw_glyph_italic(x, y, glyph_mask(c, (int)scale), W, H, color);
            else       draw_glyph(x, y, glyph_mask(c, (int)scale), W, H, color);
            continue;
        }
        const unsigned char *g = font8x16[c];              /* too big to keep: resample as it is drawn */
        for(int oy = 0; oy < H; oy++){
            int fy = ((oy * 2 + 1) << 7) / (int)scale - 128;
            int sh = italic ? ((H - 1 - oy) >> 2) : 0;
            for(int ox = 0; ox < W; ox++){
                int cov = glyph_cov(g, ((ox * 2 + 1) << 7) / (int)scale - 128, fy);
                if(cov > 0 && x + ox >= 0 && y + oy >= 0) blendpx((u32)(x + ox + sh), (u32)(y + oy), color, (u32)cov);
            }
        }
    }
}

static void blit_image(const web_cmd_t *c, const u32 *px, u32 iw, u32 ih){
    /* nearest neighbour, scaled to c->w x c->h, clipped to the viewport */
    int dw = c->w, dh = c->h;
    if(dw <= 0 || dh <= 0 || !px) return;
    int x0 = c->x, y0 = c->y;
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    int x1 = c->x + dw, y1 = c->y + dh;
    if(x1 > (int)vp_w) x1 = (int)vp_w;
    if(y1 > (int)vp_h) y1 = (int)vp_h;
    for(int dy = y0; dy < y1; dy++){
        u32 sy = (u32)(((i64)(dy - c->y) * (i64)ih) / (i64)dh);
        if(sy >= ih) sy = ih - 1;
        const u32 *row = px + (u64)sy * iw;
        u32 *dst = (u32 *)((u8 *)vram + (u64)dy * VP_PITCH * 4);
        for(int dx = x0; dx < x1; dx++){
            u32 sx = (u32)(((i64)(dx - c->x) * (i64)iw) / (i64)dw);
            if(sx >= iw) sx = iw - 1;
            dst[dx] = row[sx];
        }
    }
}

/* Paints the engine's drawing commands, scrolled by `scroll` pixels.
 * hover_node, when non-NULL, is underlined if it is a link. */
void painter_cmds(const web_cmd_t *cmds, int n, int scroll, const void *hover_node){
    for(int i = 0; i < n; i++){
        const web_cmd_t *c = &cmds[i];
        int y = c->y - scroll;
        if(c->kind == WEB_CMD_RECT){
            int x0 = c->x < 0 ? 0 : c->x, y0 = y < 0 ? 0 : y;
            int x1 = c->x + c->w, y1 = y + c->h;
            if(x1 > (int)vp_w) x1 = (int)vp_w;
            if(y1 > (int)vp_h) y1 = (int)vp_h;
            if(x1 > x0 && y1 > y0) pxfill((u32)x0, (u32)y0, (u32)(x1 - x0), (u32)(y1 - y0), c->color);
            continue;
        }
        if(c->kind == WEB_CMD_IMAGE){
            u32 iw = 0, ih = 0;
            const u32 *px = web_image_pixels(c->img, &iw, &ih);
            if(px){
                web_cmd_t sc = *c;
                sc.x = c->x; sc.y = y;                       /* already scrolled */
                blit_image(&sc, px, iw, ih);
            }
            continue;
        }
        /* text: skip lines fully outside the viewport */
        if(y < -c->h || y > (int)vp_h) continue;
        int x = c->x < 0 ? 0 : c->x;
        draw_text(x, y, c->text, c->color, (u32)c->scale, c->italic);
        if(c->bold) draw_text(x + 1, y, c->text, c->color, (u32)c->scale, c->italic);
        if(c->link >= 0 && c->dom == hover_node && y + c->h >= 2) pxfill((u32)x, (u32)(y + c->h - 2), (u32)c->w, 2, c->color);
    }
}
