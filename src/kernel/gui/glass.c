/* Glass: windows, the taskbar, menus and panels can be frosted, see-through glass. What shows
 * through is one picture - the wallpaper with the desktop icons on it, blurred - built once and
 * kept. A glass panel is that picture under it, tinted; because it never depends on what was
 * drawn there a moment ago, repainting only part of a window gives exactly the same pixels. */
#include "gui_internal.h"
#include "amdroid/fb.h"
#include "amdroid/heap.h"

#define DS 4                                     /* the blur works on a picture this much smaller */

static u32 *layer = NULL;                        /* screen-sized: first the desktop, then its blur */
static u32 *small_a = NULL, *small_b = NULL;
static u32 lw = 0, lh = 0, sw = 0, sh = 0;
static int failed = 0;
static u32 built_sig = 0;
static int have = 0;

static int clampi(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }

/* One box blur pass over 'count' lines of 'len' pixels; 'el' steps along a line, 'ln' to the next. */
static void box(const u32 *src, u32 *dst, int len, int count, int el, int ln, int r){
    int n = 2 * r + 1;
    for(int l = 0; l < count; l++){
        const u32 *s = src + (long)l * ln;
        u32 *d = dst + (long)l * ln;
        int sr = 0, sg = 0, sb = 0;
        for(int k = -r; k <= r; k++){
            u32 c = s[(long)clampi(k, 0, len - 1) * el];
            sr += (int)((c >> 16) & 255); sg += (int)((c >> 8) & 255); sb += (int)(c & 255);
        }
        for(int i = 0; i < len; i++){
            d[(long)i * el] = ((u32)(sr / n) << 16) | ((u32)(sg / n) << 8) | (u32)(sb / n);
            u32 a = s[(long)clampi(i + r + 1, 0, len - 1) * el], o = s[(long)clampi(i - r, 0, len - 1) * el];
            sr += (int)((a >> 16) & 255) - (int)((o >> 16) & 255);
            sg += (int)((a >> 8) & 255) - (int)((o >> 8) & 255);
            sb += (int)(a & 255) - (int)(o & 255);
        }
    }
}

static const int BLUR_R[3] = { 2, 4, 7 };                       /* at 1/DS size */
static const u32 TINT_A[3] = { 96, 140, 196 };                  /* 0..256: how much tint over the blur */
static const u32 TINT_C[3] = { 0x22303C, 0x1A242D, 0x0D1217 };  /* clear glass is bluish, dark glass nearly black */

static u32 glass_sig(void){
    u32 h = (u32)ui_wall_index() * 131u + (u32)ui_accent_index() * 17u + (u32)ui_prefs.glass_blur;
    return h * 1000003u + desk_signature();
}

static int build(void){
    u32 w = fb_width(), h = fb_height();
    if(!layer || lw != w || lh != h){
        int save = heap_owner; heap_owner = 0;          /* not charged to whichever app is drawing */
        layer = kmalloc(w * h * 4);
        sw = (w + DS - 1) / DS; sh = (h + DS - 1) / DS;
        small_a = kmalloc(sw * sh * 4);
        small_b = kmalloc(sw * sh * 4);
        heap_owner = save;
        lw = w; lh = h;
        if(!layer || !small_a || !small_b){ failed = 1; layer = NULL; return 0; }
    }
    /* the desktop as it is without windows */
    fb_target(layer, lw, lh);
    wall_draw();
    desk_draw_plain();
    fb_target_screen();
    /* shrink (average DS x DS blocks) */
    for(u32 y = 0; y < sh; y++)
        for(u32 x = 0; x < sw; x++){
            u32 r = 0, g = 0, b = 0, n = 0;
            for(u32 yy = y * DS; yy < y * DS + DS && yy < lh; yy++)
                for(u32 xx = x * DS; xx < x * DS + DS && xx < lw; xx++){
                    u32 c = layer[yy * lw + xx];
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++;
                }
            small_a[y * sw + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    /* blur: three passes each way come close to a gaussian */
    int rad = BLUR_R[clampi(ui_prefs.glass_blur, 0, 2)];
    for(int pass = 0; pass < 3; pass++){
        box(small_a, small_b, (int)sw, (int)sh, 1, (int)sw, rad);
        box(small_b, small_a, (int)sh, (int)sw, (int)sw, 1, rad);
    }
    /* grow back (bilinear) over the same buffer */
    for(u32 y = 0; y < lh; y++){
        int fy = clampi((int)(y * 256 / DS) - 128, 0, (int)(sh - 1) * 256);
        int y0 = fy >> 8, ty = fy & 255, y1 = y0 + 1 < (int)sh ? y0 + 1 : y0;
        for(u32 x = 0; x < lw; x++){
            int fx = clampi((int)(x * 256 / DS) - 128, 0, (int)(sw - 1) * 256);
            int x0 = fx >> 8, tx = fx & 255, x1 = x0 + 1 < (int)sw ? x0 + 1 : x0;
            u32 c00 = small_a[y0 * sw + x0], c10 = small_a[y0 * sw + x1];
            u32 c01 = small_a[y1 * sw + x0], c11 = small_a[y1 * sw + x1];
            u32 out = 0;
            for(int sft = 0; sft <= 16; sft += 8){
                int a = (int)((c00 >> sft) & 255), b = (int)((c10 >> sft) & 255);
                int c = (int)((c01 >> sft) & 255), d = (int)((c11 >> sft) & 255);
                int top = a * (256 - tx) + b * tx, bot = c * (256 - tx) + d * tx;
                out |= (u32)((top * (256 - ty) + bot * ty) >> 16) << sft;
            }
            layer[y * lw + x] = out;
        }
    }
    built_sig = glass_sig();
    have = 1;
    return 1;
}

const u32 *glass_backdrop(void){
    if(!ui_prefs.glass || failed) return NULL;
    if(!have || built_sig != glass_sig()){ if(!build()) return NULL; }
    return layer;
}

/* The tint over the blur: a dark glass with a breath of the accent colour. */
void glass_params(u32 *tint, u32 *ta){
    *tint = ui_lerp(TINT_C[clampi(ui_prefs.glass_tint, 0, 2)], ui_accent(), 14);
    *ta = TINT_A[clampi(ui_prefs.glass_tint, 0, 2)];
}

/* What is behind the glass changed (the wallpaper, an icon moved): everything drawn with it
 * is drawn again. */
void glass_tick(void){
    static u32 seen_sig = 0;
    static int seen_on = 0;
    if(!ui_prefs.glass){ seen_on = 0; return; }
    u32 s = glass_sig();
    if(seen_on && s != seen_sig) gui_damage_all();
    seen_sig = s; seen_on = 1;
}
