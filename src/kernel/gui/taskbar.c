/* Taskbar (Start, Search, pinned and open apps, Terminal, clock) and the Start menu. */
#include "gui_internal.h"

#define BAR_H       48
#define BAR_MARGIN  12                 /* gap between the taskbar and the screen bottom */
#define BAR_ITEM    52                 /* width of one taskbar slot */
#define BAR_PAD     14

static const char *terminal_label = "Terminal";

/* Which apps stay on the taskbar when closed: from each app, then changed by the user. */
static int pins[UI_MAX_APPS];
static int pins_ready = 0;
static void pins_init(void){
    if(pins_ready) return;
    for(int a = 0; a < ui_app_count(); a++) pins[a] = ui_apps[a]->pinned;
    pins_ready = 1;
}
int  tb_pinned(int a){ pins_init(); return a >= 0 && a < UI_MAX_APPS && pins[a]; }
void tb_set_pinned(int a, int on){ pins_init(); if(a >= 0 && a < UI_MAX_APPS) pins[a] = on; }

int tb_items(item_t *it){
    int n = 0;
    it[n++] = (item_t){ IT_START, 0 };
    it[n++] = (item_t){ IT_SEARCH, 0 };
    int apps = ui_app_count();
    for(int a = 0; a < apps; a++)
        if(win_is_open(a) || tb_pinned(a)) it[n++] = (item_t){ IT_APP, a };
    it[n++] = (item_t){ IT_TERM, 0 };
    return n;
}

rect_t tb_bar(int n, u32 sw, u32 sh){
    u32 w = (u32)n * BAR_ITEM + 2 * BAR_PAD;
    return (rect_t){ (sw - w) / 2, sh - BAR_MARGIN - BAR_H, w, BAR_H };
}
rect_t tb_item(rect_t bar, int i){
    return (rect_t){ bar.x + BAR_PAD + (u32)i * BAR_ITEM, bar.y + 6, BAR_ITEM - 8, BAR_H - 12 };
}
int tb_hit(rect_t bar, int n, int x, int y){
    for(int i = 0; i < n; i++)
        if(ui_contains(tb_item(bar, i), x, y)) return i;
    return -1;
}

void tb_draw(const item_t *it, int n, rect_t bar, int hover, int hover_amt, int top, int start_open, u32 sw){
    (void)sw;
    ui_rrect(bar.x, bar.y, bar.w, bar.h, bar.h / 2, C_BAR_EDGE);
    ui_rrect(bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2, (bar.h - 2) / 2, C_BAR);
    for(int i = 0; i < n; i++){
        rect_t r = tb_item(bar, i);
        if(i == hover) ui_rrect(r.x, r.y, r.w, r.h, 10, ui_lerp(C_BAR, C_HOVER, hover_amt));
        u32 col = C_TEXT;
        int ico;
        if(it[i].kind == IT_START){ ico = UI_ICO_START; if(start_open) col = ui_accent(); }
        else if(it[i].kind == IT_SEARCH){ ico = UI_ICO_SEARCH; }
        else if(it[i].kind == IT_TERM){ ico = UI_ICO_TERM; }
        else {
            ico = ui_apps[it[i].app]->icon;
            if(it[i].app == top) col = ui_accent();
        }
        ui_icon(ico, r.x + (r.w - 32) / 2, r.y + 2, col);
        if(it[i].kind == IT_APP && win_is_open(it[i].app)){
            int focused = it[i].app == top;
            u32 dot_w = focused ? 18 : 6;
            ui_rrect(r.x + (r.w - dot_w) / 2, r.y + r.h - 5, dot_w, 3, 1,
                     focused ? ui_accent() : C_MUTED);
        }
    }
    /* the clock at the right edge, centred on the bar (signed maths: the bar is lower than a 2x line) */
    char clk[8];
    ui_clock(clk);
    u32 cw = ui_text_w(clk, UI_SCALE);
    int ty = (int)bar.y + ((int)bar.h - (int)UI_CH) / 2;
    ui_text(sw - cw - 32, (u32)ty, clk, C_MUTED, UI_SCALE);

    /* a label over the icon under the pointer, once the hover has faded in */
    if(hover >= 0 && hover < n && hover_amt > 160){
        const char *label = it[hover].kind == IT_START ? "Start" : it[hover].kind == IT_SEARCH ? "Search" :
                            it[hover].kind == IT_TERM ? "Terminal" : ui_apps[it[hover].app]->name;
        rect_t r = tb_item(bar, hover);
        u32 lw = ui_text_w(label, 1) + 24;
        u32 lx = r.x + r.w / 2 - lw / 2;
        u32 ly = bar.y - 40;
        ui_rrect(lx, ly, lw, 30, 8, C_MENU);
        ui_text(lx + 12, ly + 7, label, C_TEXT, 1);
    }
}

/* ---- Start menu: every app, plus Terminal, above the taskbar ---- */
#define MENU_W    380
#define MENU_ROW  44
#define MENU_TOP  56
#define MENU_SEP  16                   /* the gap (with a line) before Terminal and Sign out */
rect_t tb_menu(rect_t bar, u32 sw){
    int rows = ui_app_count() + 2;
    u32 h = MENU_TOP + (u32)rows * MENU_ROW + MENU_SEP + 12;
    return (rect_t){ (sw - MENU_W) / 2, bar.y - 16 - h, MENU_W, h };
}
static rect_t menu_row(rect_t m, int i){
    u32 extra = i >= ui_app_count() ? MENU_SEP : 0;
    return (rect_t){ m.x + 10, m.y + MENU_TOP + (u32)i * MENU_ROW + extra, m.w - 20, MENU_ROW - 4 };
}
int tb_menu_hit(rect_t m, int x, int y){
    int apps = ui_app_count();
    for(int i = 0; i < apps + 2; i++)
        if(ui_contains(menu_row(m, i), x, y)){
            if(i < apps) return i;
            return i == apps ? UI_MAX_APPS : UI_MAX_APPS + 1;   /* Terminal, Sign out */
        }
    return -1;
}
void tb_menu_draw(rect_t bar, u32 sw, int dy, int mx, int my){
    rect_t m = tb_menu(bar, sw);
    int hot = tb_menu_hit(m, mx, my);         /* (before the slide offset: the row under the pointer) */
    m.y += (u32)dy;
    ui_rrect(m.x + 6, m.y + 10, m.w, m.h, 16, C_SHADOW);
    ui_rrect(m.x, m.y, m.w, m.h, 16, C_MENU);
    ui_text(m.x + 24, m.y + 16, "Apps", C_MUTED, UI_SCALE);
    int apps = ui_app_count();
    /* the line between the apps and Terminal / Sign out */
    u32 sep_y = m.y + MENU_TOP + (u32)apps * MENU_ROW + MENU_SEP / 2 - 2;
    ui_fill((rect_t){ m.x + 20, sep_y, m.w - 40, 1 }, C_SEP);
    for(int i = 0; i <= apps + 1; i++){
        rect_t r = menu_row(m, i);
        int id = i < apps ? i : (i == apps ? UI_MAX_APPS : UI_MAX_APPS + 1);
        if(id == hot){
            ui_rrect(r.x, r.y, r.w, r.h, 10, C_HOVER);
            ui_rrect(r.x, r.y + 8, 3, r.h - 16, 1, ui_accent());
        }
        u32 ty = r.y + (r.h - UI_CH) / 2;
        if(i == apps + 1){
            ui_text(r.x + 56, ty, "Sign out", C_WARN, UI_SCALE);
        } else if(i < apps){
            ui_icon(ui_apps[i]->icon, r.x + 12, r.y + (r.h - 32) / 2, C_TEXT);
            ui_text(r.x + 56, ty, ui_apps[i]->name, C_TEXT, UI_SCALE);
        } else {
            ui_icon(UI_ICO_TERM, r.x + 12, r.y + (r.h - 32) / 2, C_TEXT);
            ui_text(r.x + 56, ty, terminal_label, C_TEXT, UI_SCALE);
        }
    }
}
