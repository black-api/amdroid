/* Files: browse the disk, open folders, read text files. */
#include "amdroid/ui.h"
#include "amdroid/fat32.h"
#include "amdroid/heap.h"
#include "amdroid/keyboard.h"

#define NAME_LEN   15
#define PATH_LEN   120
#define FILE_MAX   64
#define VIEW_LINES 4096
#define VIEW_BYTES (64 * 1024)

typedef struct { char name[NAME_LEN + 1]; u32 size; int is_dir; } entry_t;

static char path[PATH_LEN];          /* current folder, "" is the root */
static entry_t ents[FILE_MAX];
static int nents = 0, sel = 0, top = 0;
static rect_t last_body;            /* the body from the last draw, for key handling */

/* viewer */
static char *vbuf = NULL;
static u32 vlen = 0, vsize = 0;
static u32 vstarts[VIEW_LINES];
static int vnlines = 0, vtop = 0, vbinary = 0;
static char vname[NAME_LEN + 1];

static void ent_cb(const char *name, int is_dir, u32 size){
    if(nents >= FILE_MAX) return;
    entry_t *e = &ents[nents++];
    ui_copy(e->name, name, NAME_LEN + 1);
    e->is_dir = is_dir;
    e->size = size;
}

static void load(void){
    nents = 0; sel = 0; top = 0;
    fat32_list_path(path, ent_cb);
}

static void move(int d, int rows){
    if(nents == 0) return;
    sel += d;
    if(sel < 0) sel = 0;
    if(sel > nents - 1) sel = nents - 1;
    if(sel < top) top = sel;
    if(sel >= top + rows) top = sel - rows + 1;
}

static void up(void){
    int n = (int)ui_len(path);
    if(n == 0) return;
    int start = n;
    while(start > 0 && path[start - 1] != '/') start--;
    char child[NAME_LEN + 1];
    ui_copy(child, path + start, sizeof child);
    if(start > 0) start--;
    path[start] = 0;
    load();
    for(int i = 0; i < nents; i++)
        if(ui_streq(ents[i].name, child)){
            sel = i;
            top = i >= ui_list_rows(last_body) ? i - ui_list_rows(last_body) + 1 : 0;
            break;
        }
}

static void viewer_close(void){
    if(vbuf) kfree(vbuf);
    vbuf = NULL; vlen = 0; vnlines = 0; vtop = 0;
}

static void viewer_open(const char *name){
    char full[PATH_LEN + NAME_LEN + 2] = "";
    if(path[0]){ ui_cat(full, path); ui_cat(full, "/"); }
    ui_cat(full, name);

    viewer_close();
    vbuf = kmalloc(VIEW_BYTES + 1);
    if(!vbuf){ ui_msg_set("Out of memory."); return; }
    u32 size = 0;
    if(fat32_read_path(full, vbuf, VIEW_BYTES, &size) != 0){
        kfree(vbuf); vbuf = NULL;
        ui_msg_set("Cannot read that file.");
        return;
    }
    ui_copy(vname, name, sizeof vname);
    vsize = size;
    vlen = size < VIEW_BYTES ? size : VIEW_BYTES;
    vbinary = 0;
    for(u32 i = 0; i < vlen && i < 512; i++){
        u8 c = (u8)vbuf[i];
        if(c < 9 || (c > 13 && c < 32)){ vbinary = 1; break; }
    }
    vnlines = 0;
    if(!vbinary && vlen > 0){
        vstarts[vnlines++] = 0;
        for(u32 i = 0; i < vlen && vnlines < VIEW_LINES; i++)
            if(vbuf[i] == '\n' && i + 1 < vlen) vstarts[vnlines++] = i + 1;
    }
    vtop = 0;
}

static int viewer_rows(rect_t b){ return ((int)b.h - 64 - UI_BTN_H - 8) / UI_CH; }
static rect_t back_btn(rect_t b){ return ui_button(b, 150); }

/* Moves the viewer by d lines, as far as the text goes. */
static void viewer_scroll(int d){
    int m = vnlines - viewer_rows(last_body);
    vtop += d;
    if(vtop > m) vtop = m;
    if(vtop < 0) vtop = 0;
}

static void activate(void){
    if(nents == 0) return;
    entry_t *e = &ents[sel];
    if(e->is_dir){
        u32 n = ui_len(path);
        if(n + 1 + ui_len(e->name) + 1 > PATH_LEN){
            ui_msg_set("Path is too deep.");
            return;
        }
        if(n > 0) ui_cat(path, "/");
        ui_cat(path, e->name);
        load();
    } else {
        viewer_open(e->name);
    }
}

/* ---- the app interface ---- */

static void f_open(void){
    load();
}

static void f_close(void){
    viewer_close();
}

static void draw_viewer(rect_t b){
    rect_t bb = back_btn(b);
    ui_rrect(bb.x, bb.y, bb.w, bb.h, 10, C_CARD_HI);
    ui_text(bb.x + (bb.w - ui_text_w("Back", UI_SCALE)) / 2, bb.y + 4, "Back", C_TEXT, UI_SCALE);
    ui_text(b.x, b.y + 4, vname, C_TEXT, UI_SCALE);
    b.y += UI_BTN_H + 8; b.h -= UI_BTN_H + 8;        /* the text goes under that row */
    if(vbinary){
        char m[48] = "(binary file, ", n[12];
        ui_num(vsize, n); ui_cat(m, n); ui_cat(m, " bytes)");
        ui_text(b.x + 16, b.y + 16, m, C_MUTED, UI_SCALE);
    } else if(vnlines == 0){
        ui_text(b.x + 16, b.y + 16, "(empty file)", C_MUTED, UI_SCALE);
    }
    int cols = (int)(b.w - 32) / (int)UI_CW;
    if(cols > 120) cols = 120;
    int rows = viewer_rows(last_body);
    for(int i = 0; i < rows && !vbinary; i++){
        int ln = vtop + i;
        if(ln >= vnlines) break;
        u32 s = vstarts[ln];
        u32 e = (ln + 1 < vnlines) ? vstarts[ln + 1] : vlen;
        char t[124];
        int k = 0;
        for(u32 j = s; j < e && k < cols; j++){
            char c = vbuf[j];
            if(c == '\n' || c == '\r') break;
            if(c == '\t') c = ' ';
            if((u8)c < 32 || (u8)c > 126) c = '.';
            t[k++] = c;
        }
        t[k] = 0;
        ui_text(b.x + 16, b.y + 16 + i * UI_CH, t, C_TEXT, UI_SCALE);
    }
    if(!vbinary && vnlines > 0){
        char foot[80] = "", a[12], c2[12], c3[12];
        ui_num((u32)vtop + 1, a);
        u32 last = (u32)vtop + (u32)rows;
        if(last > (u32)vnlines) last = (u32)vnlines;
        ui_num(last, c2); ui_num((u32)vnlines, c3);
        ui_cat(foot, "Lines "); ui_cat(foot, a);
        ui_cat(foot, "-"); ui_cat(foot, c2); ui_cat(foot, " of "); ui_cat(foot, c3);
        if(vsize > VIEW_BYTES) ui_cat(foot, "  (the first 64 KB of the file)");
        ui_text(b.x, b.y + b.h - 24, foot, C_DIM, 1);
    }
}

static void f_draw(rect_t b, int focused){
    (void)focused;
    last_body = b;
    if(vbuf){ draw_viewer(b); return; }

    char p[PATH_LEN + 4] = "/";
    ui_cat(p, path);
    ui_text(b.x, b.y + 4, p, C_TEXT, UI_SCALE);
    char cnt[24] = "";
    ui_num((u32)nents, cnt);
    ui_cat(cnt, nents == 1 ? " item" : " items");
    ui_text(b.x + ui_text_w(p, UI_SCALE) + 20, b.y + 14, cnt, C_DIM, 1);
    rect_t upb = ui_button(b, 150);
    ui_rrect(upb.x, upb.y, upb.w, upb.h, 10, C_CARD_HI);
    ui_text(upb.x + (upb.w - ui_text_w("Up", UI_SCALE)) / 2, upb.y + 4, "Up", path[0] ? C_TEXT : C_DIM, UI_SCALE);

    rect_t hd = ui_list_head(b);
    ui_text(hd.x + 20, hd.y + 4, "Name", C_DIM, UI_SCALE);
    ui_text_right((rect_t){ hd.x, hd.y, hd.w - 20, hd.h }, hd.y + 4, "Size", C_DIM, UI_SCALE);

    int rows = ui_list_rows(b);
    for(int i = 0; i < rows; i++){
        int idx = top + i;
        if(idx >= nents) break;
        rect_t r = ui_list_row(b, i);
        int hot = idx == sel;
        ui_fill(r, hot ? C_CARD_HI : C_CARD);
        if(hot) ui_fill((rect_t){ r.x, r.y, 4, r.h }, ui_accent());
        u32 ty = r.y + (r.h - UI_CH) / 2;
        ui_text(r.x + 20, ty, ents[idx].name, ents[idx].is_dir ? ui_accent() : C_TEXT, UI_SCALE);
        char sz[16] = "";
        if(ents[idx].is_dir) ui_cat(sz, "<DIR>");
        else { ui_num(ents[idx].size, sz); ui_cat(sz, " B"); }
        ui_text_right((rect_t){ r.x, r.y, r.w - 20, r.h }, ty, sz, C_MUTED, UI_SCALE);
    }
    if(nents == 0)
        ui_text(b.x + 20, ui_list_row(b, 0).y + 4, "(this folder is empty)", C_MUTED, UI_SCALE);
    if(nents > rows){
        /* a scrollbar: where the visible rows sit in the whole list */
        rect_t first = ui_list_row(b, 0);
        u32 track_h = (u32)rows * UI_ROW_H;
        u32 th = track_h * (u32)rows / (u32)nents;
        if(th < 24) th = 24;
        u32 ty = first.y + (track_h - th) * (u32)top / (u32)(nents - rows);
        ui_fill((rect_t){ b.x + b.w - 5, first.y, 3, track_h }, C_SEP);
        ui_rrect(b.x + b.w - 6, ty, 5, th, 2, C_MUTED);
    }
}

static int f_key(int c){
    int rows = ui_list_rows(last_body);
    if(vbuf){
        if(c == 27 || c == '\b'){ viewer_close(); return 1; }
        if(c == KEY_UP)   { viewer_scroll(-1); return 1; }
        if(c == KEY_DOWN) { viewer_scroll(1); return 1; }
        if(c == KEY_PGUP) { viewer_scroll(-viewer_rows(last_body)); return 1; }
        if(c == KEY_PGDN) { viewer_scroll(viewer_rows(last_body)); return 1; }
        if(c == KEY_HOME) { viewer_scroll(-vnlines); return 1; }
        if(c == KEY_END)  { viewer_scroll(vnlines); return 1; }
        return 0;
    }
    if(c == '\b'){ up(); return 1; }
    if(c == '\n'){ activate(); return 1; }
    if(c == KEY_UP)   { move(-1, rows); return 1; }
    if(c == KEY_DOWN) { move(1, rows); return 1; }
    if(c == KEY_PGUP) { move(-rows, rows); return 1; }
    if(c == KEY_PGDN) { move(rows, rows); return 1; }
    return 0;
}

static void f_click(rect_t b, int x, int y){
    if(vbuf){
        if(ui_contains(back_btn(b), x, y)) viewer_close();
        return;
    }
    if(ui_contains(ui_button(b, 150), x, y)){ up(); return; }
    int rows = ui_list_rows(b);
    for(int i = 0; i < rows; i++){
        if(ui_contains(ui_list_row(b, i), x, y) && top + i < nents){
            if(top + i == sel) activate();
            else sel = top + i;
            return;
        }
    }
}

/* The wheel: three lines of a file, or three rows of the list. */
static void f_scroll(rect_t b, int x, int y, int dz){
    (void)x; (void)y;
    if(vbuf){ viewer_scroll(dz * 3); return; }
    int rows = ui_list_rows(b);
    top += dz * 3;
    if(top > nents - rows) top = nents - rows;
    if(top < 0) top = 0;
    if(sel < top) sel = top;                          /* the selection stays among the rows shown */
    if(sel >= top + rows) sel = top + rows - 1;
}

const app_t app_files = {
    .name = "Files",
    .icon = UI_ICO_FILES,
    .open = f_open,
    .close = f_close,
    .draw = f_draw,
    .key = f_key,
    .click = f_click,
    .scroll = f_scroll,
};
