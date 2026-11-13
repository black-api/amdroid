/* Context menus (right click): a card of items at the pointer. A click on an item gives
 * its action to the caller; a click anywhere else, or Esc, closes it. */
#include "gui_internal.h"

#define CM_ROW   44
#define CM_SEP   13
#define CM_PAD   8
#define CM_OPEN_TICKS 10

static cm_item_t items[CM_MAX];
static int nitems = 0;
static int is_open = 0;
static int ctx_val = -1;
static rect_t card;
static u64 opened_at = 0;

static int item_h(int i){ return CM_ROW + (items[i].sep ? CM_SEP : 0); }

void cm_open(int x, int y, const cm_item_t *list, int n, int ctx, u32 sw, u32 sh, u64 now){
    if(n > CM_MAX) n = CM_MAX;
    nitems = n;
    u32 w = 240;
    int h = 2 * CM_PAD;
    for(int i = 0; i < n; i++){
        items[i] = list[i];
        u32 tw = ui_text_w(items[i].label, UI_SCALE) + 56;
        if(tw > w) w = tw;
        h += item_h(i);
    }
    /* keep it on the screen: open up or left when there is no room */
    int cx = x + 2, cy = y + 2;
    if(cx + (int)w > (int)sw - 8) cx = x - (int)w - 2;
    if(cy + h > (int)sh - 8) cy = y - h - 2;
    if(cx < 8) cx = 8;
    if(cy < 8) cy = 8;
    card = (rect_t){ (u32)cx, (u32)cy, w, (u32)h };
    ctx_val = ctx;
    is_open = 1;
    opened_at = now;
}

int  cm_is_open(void){ return is_open; }
void cm_close(void){ is_open = 0; }

/* The item under (x, y), or -1. */
static int item_at(int x, int y){
    if(!ui_contains(card, x, y)) return -1;
    int top = (int)card.y + CM_PAD;
    for(int i = 0; i < nitems; i++){
        int ry = top + (items[i].sep ? CM_SEP : 0);
        if(y >= ry && y < ry + CM_ROW) return i;
        top += item_h(i);
    }
    return -1;
}

/* A left click while the menu is open: closes it. Returns 1 and the action when an
 * enabled item was clicked. */
int cm_click(int x, int y, int *action, int *ctx){
    int i = item_at(x, y);
    int inside = ui_contains(card, x, y);
    if(inside && (i < 0 || !items[i].enabled)) return 0;     /* on a separator or a grey item: stay open */
    is_open = 0;
    if(i < 0) return 0;
    *action = items[i].action;
    *ctx = ctx_val;
    return 1;
}
int cm_inside(int x, int y){ return is_open && ui_contains(card, x, y); }

/* Opening: it drops in and fades from the desktop. 255 once it is in place. */
static int open_amount(u64 now){
    return ui_anim_on && now - opened_at < CM_OPEN_TICKS ? ui_ease((int)((now - opened_at) * 255 / CM_OPEN_TICKS)) : 255;
}

void cm_damage(int mx, int my, u64 now){
    static int sh_open = 0, sh_t = 0, sh_hov = -1;
    static rect_t sh_card;
    int t = is_open ? open_amount(now) : 0;
    int hov = is_open ? item_at(mx, my + (255 - t) * 10 / 255) : -1;
    if(is_open == sh_open && t == sh_t && hov == sh_hov && card.x == sh_card.x && card.y == sh_card.y) return;
    /* the card with its shadow, and the 10 pixels above that it drops through */
    if(sh_open) gui_damage((int)sh_card.x, (int)sh_card.y - 10, (int)sh_card.w + 3, (int)sh_card.h + 18);
    if(is_open) gui_damage((int)card.x, (int)card.y - 10, (int)card.w + 3, (int)card.h + 18);
    sh_open = is_open; sh_t = t; sh_hov = hov; sh_card = card;
}

void cm_draw(int mx, int my, u64 now){
    if(!is_open) return;
    int t = open_amount(now);
    int dy = (255 - t) * 10 / 255;
    rect_t c = card;
    c.y = (u32)((int)c.y - dy);
    ui_rrect(c.x + 3, c.y + 8, c.w, c.h, 14, C_SHADOW);
    ui_rrect(c.x, c.y, c.w, c.h, 14, ui_lerp(C_WIN, C_BAR_EDGE, t));
    ui_rrect(c.x + 1, c.y + 1, c.w - 2, c.h - 2, 13, C_MENU);
    int hov = item_at(mx, my + dy);
    int top = (int)c.y + CM_PAD;
    for(int i = 0; i < nitems; i++){
        if(items[i].sep){
            ui_fill((rect_t){ c.x + 16, (u32)(top + CM_SEP / 2), c.w - 32, 1 }, C_SEP);
            top += CM_SEP;
        }
        if(i == hov && items[i].enabled){
            ui_rrect(c.x + 6, (u32)top, c.w - 12, CM_ROW, 10, C_HOVER);
            ui_fill((rect_t){ c.x + 6, (u32)top + 10, 3, CM_ROW - 20 }, ui_accent());
        }
        u32 col = !items[i].enabled ? C_DIM : items[i].danger ? 0xFF7A6B : C_TEXT;
        ui_text(c.x + 24, (u32)top + (CM_ROW - UI_CH) / 2, items[i].label, ui_lerp(C_MENU, col, t), UI_SCALE);
        top += CM_ROW;
    }
}
