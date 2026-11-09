/* painter.c — draws the engine's command list into the viewport buffer that
 * the shared region exposes. The page is painted by Husser, not by the GUI:
 * the browser only blits the result. Same anti-aliased text as the desktop. */
#include "amdroid/husser_proto.h"
#include "amdroid/web.h"
#include "amdroid/font.h"
#include "string.h"

extern volatile husser_shm_t *husser_vshm;             /* set by husser.c */
extern volatile u32 *husser_vram;

static u32 vp_w, vp_h;
#define VP_PITCH (vp_w)

static void pxfill(u32 x, u32 y, u32 w, u32 h, u32 c){
    if(!vp_w) return;
    u32 x1 = x + w, y1 = y + h;
    if(x >= vp_w || y >= vp_h) return;
    if(x1 > vp_w) x1 = vp_w;
    if(y1 > vp_h) y1 = vp_h;
    for(u32 yy = y; yy < y1; yy++){
        u32 *row = (u32 *)((u8 *)husser_vram + (u64)yy * VP_PITCH * 4) + x;
        for(u32 xx = x; xx < x1; xx++) *row++ = c;
    }
}
static void blendpx(u32 x, u32 y, u32 c, u32 a){
    if(x >= vp_w || y >= vp_h || !a) return;
    u32 *p = &husser_vram[y * VP_PITCH + x];
    if(a >= 256){ *p = c; return; }
    u32 bg = *p, ia = 256 - a;
    u32 rb = ((((bg & 0x00FF00FF) * ia + (c & 0x00FF00FF) * a) >> 8) & 0x00FF00FF);
    u32 g  = ((((bg & 0x0000FF00) * ia + (c & 0x0000FF00) * a) >> 8) & 0x0000FF00);
    *p = rb | g;
}

void painter_set_size(u32 w, u32 h){ vp_w = w; vp_h = h; }

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
static void draw_text(u32 x, u32 y, const char *s, u32 color, u32 scale){
    if(!scale) scale = 1;
    int W = FONT_W * (int)scale, H = FONT_H * (int)scale;
    u32 cx = x;
    for(; *s; s++, cx += (u32)W){
        unsigned char c = (unsigned char)*s;
        if(c > 127) c = '?';
        const unsigned char *g = font8x16[c];
        for(int oy = 0; oy < H; oy++){
            int fy = ((oy * 2 + 1) << 7) / (int)scale - 128;
            for(int ox = 0; ox < W; ox++){
                int fx = ((ox * 2 + 1) << 7) / (int)scale - 128;
                int cov = glyph_cov(g, fx, fy);
                if(cov > 0) blendpx(cx + (u32)ox, y + (u32)oy, color, (u32)cov);
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
        u32 *dst = (u32 *)((u8 *)husser_vram + (u64)dy * VP_PITCH * 4);
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
        u32 x = (u32)(c->x < 0 ? 0 : c->x);
        draw_text(x, (u32)y, c->text, c->color, (u32)c->scale);
        if(c->bold) draw_text(x + 1, (u32)y, c->text, c->color, (u32)c->scale);
        if(c->link >= 0 && c->dom == hover_node) pxfill(x, (u32)(y + c->h - 2), (u32)c->w, 2, c->color);
    }
}
