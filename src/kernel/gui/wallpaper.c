/* Wallpapers: built once into a buffer, copied to the screen each frame. */
#include "amdroid/ui.h"
#include "amdroid/fb.h"
#include "amdroid/heap.h"
#include "gui_internal.h"

#define NWALL 3
static const char *wall_names[NWALL] = { "Slate", "Night", "Ocean" };
static const u32 wall_swatch[NWALL] = { 0x3E4A54, 0x1A2034, 0x1F4A63 };
static int wall_idx = 0;

static u32 *wall = NULL;        /* the built picture, screen-sized */
static u32 wall_w = 0, wall_h = 0;
static int built = -1;         /* which wallpaper is in the buffer */

int         ui_wall_count(void){ return NWALL; }
int         ui_wall_index(void){ return wall_idx; }
const char *ui_wall_name(int i){ return wall_names[i]; }
u32         ui_wall_swatch(int i){ return wall_swatch[i]; }

static u32 clamp255(int v){ return v < 0 ? 0 : (v > 255 ? 255 : (u32)v); }

/* Adds a soft light stroke along a quadratic curve (integer maths). */
static void stroke(int x0, int y0, int x1, int y1, int x2, int y2,
                   u32 radius, u32 power, u32 tint){
    u32 sw = wall_w, sh = wall_h;
    for(int i = 0; i <= 400; i++){
        long long t = i * 1000 / 400;                      /* 0..1000 */
        long long u = 1000 - t;
        long long px = (u * u * x0 + 2 * u * t * x1 + t * t * x2) / 1000000;
        long long py = (u * u * y0 + 2 * u * t * y1 + t * t * y2) / 1000000;
        long long taper = (t < 500 ? t : 1000 - t);        /* fade both ends */
        long long R = (long long)radius * taper / 500;
        if(R < 2) continue;
        for(long long dy = -R; dy <= R; dy++){
            long long yy = py + dy;
            if(yy < 0 || yy >= (long long)sh) continue;
            for(long long dx = -R; dx <= R; dx++){
                long long xx = px + dx;
                if(xx < 0 || xx >= (long long)sw) continue;
                long long d2 = dx * dx + dy * dy;
                if(d2 >= R * R) continue;
                u32 f = (u32)((R * R - d2) * 255 / (R * R));
                u32 add = power * f * f / (255 * 255);
                u32 *p = &wall[yy * sw + xx];
                u32 pr = (*p >> 16) & 255, pg = (*p >> 8) & 255, pb = *p & 255;
                u32 tr = (tint >> 16) & 255, tg = (tint >> 8) & 255, tb = tint & 255;
                pr = clamp255((int)(pr + tr * add / 255));
                pg = clamp255((int)(pg + tg * add / 255));
                pb = clamp255((int)(pb + tb * add / 255));
                *p = (pr << 16) | (pg << 8) | pb;
            }
        }
    }
}

static void build(int style){
    if(!wall) return;
    u32 c0, c1, tint;
    if(style == 1){ c0 = 0x1A2034; c1 = 0x05060B; tint = 0x8C86FF; }      /* Night */
    else if(style == 2){ c0 = 0x1F4A63; c1 = 0x06121E; tint = 0x8FC6FF; } /* Ocean */
    else { c0 = 0x3E4A54; c1 = 0x0E1215; tint = 0xC8D2DA; }               /* Slate */

    /* diagonal gradient */
    for(u32 y = 0; y < wall_h; y++)
        for(u32 x = 0; x < wall_w; x++){
            int t = (int)((x * 255 / wall_w + y * 255 / wall_h) / 2);
            wall[y * wall_w + x] = ui_lerp(c0, c1, t);
        }
    /* soft light swooshes in the lower right */
    for(int k = 0; k < 3; k++){
        int x0 = (int)(wall_w * 54 / 100), y0 = (int)(wall_h * (84 + k * 2) / 100);
        int x1 = (int)(wall_w * (80 + k * 2) / 100), y1 = (int)(wall_h * (56 + k * 8) / 100);
        int x2 = (int)(wall_w * 101 / 100), y2 = (int)(wall_h * (70 + k * 5) / 100);
        u32 radius = wall_h * (34 - 8 * k) / 1000 + 6;
        stroke(x0, y0, x1, y1, x2, y2, radius, 34 - 8 * k, tint);
    }
    built = style;
}

void ui_wall_set(int i){
    if(i < 0 || i >= NWALL || i == wall_idx) return;
    wall_idx = i;
    build(i);
    gui_damage_all();
}

void wall_init(u32 w, u32 h){
    wall_w = w; wall_h = h;
    if(!wall) wall = kmalloc(w * h * 4);
    if(built != wall_idx) build(wall_idx);     /* kept from the login screen to the desktop */
}

void wall_draw(void){
    if(wall && built == wall_idx) fb_blit(wall);
    else fb_clear(0x000000);
}
