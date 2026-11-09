/* Desktop icons: a tile for each app on a grid down the left of the screen.
 * Click selects one, double-click opens it, and dragging moves it to another grid cell
 * (swapping with an icon already there). Positions last for the session. */
#include "gui_internal.h"

#define DK_X0     28
#define DK_Y0     92          /* below the Dynamic Island */
#define DK_CELL_W 128
#define DK_CELL_H 124
#define DK_TILE   64

static int cell_of[UI_MAX_APPS];      /* grid cell of each app's icon */
static int rows = 6;
static u32 scr_w, scr_h;
static int sel = -1;                  /* selected icon (an app index) */
static int press_app = -1;            /* pressed, maybe about to be dragged */
static int press_x, press_y;
static int dragging = 0;
static int last_app = -1;             /* for double-clicks */
static u64 last_at = 0;

void desk_sort(void){
    for(int a = 0; a < UI_MAX_APPS; a++) cell_of[a] = a;
}

void desk_init(u32 sw, u32 sh){
    scr_w = sw; scr_h = sh;
    rows = ((int)sh - DK_Y0 - 48 - 2 * 12 - 16) / DK_CELL_H;
    if(rows < 1) rows = 1;
    static int done = 0;
    if(!done){ desk_sort(); done = 1; }     /* keep the arrangement across sign-ins */
}

static rect_t cell_rect(int c){
    return (rect_t){ (u32)(DK_X0 + (c / rows) * DK_CELL_W), (u32)(DK_Y0 + (c % rows) * DK_CELL_H), DK_CELL_W, DK_CELL_H };
}
static rect_t hit_rect(int c){
    rect_t r = cell_rect(c);
    return (rect_t){ r.x + 8, r.y + 4, r.w - 16, r.h - 12 };
}

/* The cell under (x, y), or -1. */
static int cell_at(int x, int y){
    if(x < DK_X0 || y < DK_Y0) return -1;
    int col = (x - DK_X0) / DK_CELL_W, row = (y - DK_Y0) / DK_CELL_H;
    if(row >= rows) return -1;
    if(DK_X0 + (col + 1) * DK_CELL_W > (int)scr_w) return -1;
    return col * rows + row;
}

int desk_hit(int x, int y){
    for(int a = 0; a < ui_app_count(); a++)
        if(ui_contains(hit_rect(cell_of[a]), x, y)) return a;
    return -1;
}

void desk_select(int a){ sel = a; }

/* A press on the desktop. Returns the app to open (a double-click), or -1. */
int desk_press(int x, int y, u64 now){
    int a = desk_hit(x, y);
    sel = a;
    if(a < 0){ last_app = -1; return -1; }
    if(a == last_app && now - last_at < 40){ last_app = -1; press_app = -1; return a; }
    last_app = a;
    last_at = now;
    press_app = a;
    press_x = x; press_y = y;
    dragging = 0;
    return -1;
}

/* The pointer and button each frame: drags an icon, and drops it on release. */
void desk_update(int x, int y, int down){
    if(press_app < 0) return;
    if(down){
        int d = (x > press_x ? x - press_x : press_x - x) + (y > press_y ? y - press_y : press_y - y);
        if(d > 8) dragging = 1;
        return;
    }
    if(dragging){
        int c = cell_at(x, y);
        if(c >= 0 && c != cell_of[press_app]){
            for(int b = 0; b < ui_app_count(); b++)
                if(b != press_app && cell_of[b] == c){ cell_of[b] = cell_of[press_app]; break; }   /* swap */
            cell_of[press_app] = c;
        }
        last_app = -1;                  /* a drag is not half of a double-click */
    }
    press_app = -1;
    dragging = 0;
}

static void tile(int a, u32 x, u32 y, int selected, int hover){
    /* the cell's highlight */
    if(selected || hover){
        u32 bg = selected ? ui_lerp(0x1A2026, ui_accent(), 70) : 0x252E35;
        ui_rrect(x + 8, y + 4, DK_CELL_W - 16, DK_CELL_H - 12, 14, bg);
    }
    /* the icon on a rounded tile with a shadow */
    u32 tx = x + (DK_CELL_W - DK_TILE) / 2, ty = y + 12;
    ui_rrect(tx + 2, ty + 4, DK_TILE, DK_TILE, 16, C_SHADOW);
    ui_rrect(tx, ty, DK_TILE, DK_TILE, 16, ui_lerp(0x2A343C, ui_accent(), 60));
    ui_rrect(tx + 1, ty + 1, DK_TILE - 2, DK_TILE - 2, 15, 0x242D34);
    ui_icon(ui_apps[a]->icon, tx + (DK_TILE - 32) / 2, ty + (DK_TILE - 32) / 2, ui_accent());
    /* the name under it, with a shadow so it reads on any wallpaper */
    char name[16];
    ui_copy(name, ui_apps[a]->name, 14);
    if(ui_len(ui_apps[a]->name) > 13){ name[11] = '.'; name[12] = '.'; name[13] = 0; }
    u32 w = ui_text_w(name, 1);
    u32 lx = x + (DK_CELL_W - w) / 2, ly = ty + DK_TILE + 12;
    ui_text(lx + 1, ly + 1, name, 0x000000, 1);
    ui_text(lx, ly, name, C_TEXT, 1);
}

void desk_draw(int mx, int my){
    int n = ui_app_count();
    int hov = desk_hit(mx, my);
    for(int a = 0; a < n; a++){
        if(dragging && a == press_app) continue;
        rect_t r = cell_rect(cell_of[a]);
        tile(a, r.x, r.y, a == sel, a == hov && !dragging);
    }
    if(dragging && press_app >= 0){
        /* where it will land, then the icon under the pointer */
        int c = cell_at(mx, my);
        if(c >= 0){
            rect_t r = cell_rect(c);
            u32 ac = ui_accent();
            ui_fill((rect_t){ r.x + 8, r.y + 4, r.w - 16, 2 }, ac);
            ui_fill((rect_t){ r.x + 8, r.y + r.h - 10, r.w - 16, 2 }, ac);
            ui_fill((rect_t){ r.x + 8, r.y + 4, 2, r.h - 12 }, ac);
            ui_fill((rect_t){ r.x + r.w - 10, r.y + 4, 2, r.h - 12 }, ac);
        }
        int x = mx - DK_CELL_W / 2, y = my - 40;
        if(x < 0) x = 0;
        if(y < 0) y = 0;
        tile(press_app, (u32)x, (u32)y, 1, 0);
    }
}
