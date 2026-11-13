/* App Store: install .adp packages from the disk, launch installed apps and
 * remove them. Installed apps are Fuss programs; launching one runs it full
 * screen until it exits (Esc), then the desktop is repainted. */
#include "amdroid/ui.h"
#include "amdroid/font.h"
#include "amdroid/pkg.h"
#include "amdroid/fuss.h"
#include "amdroid/keyboard.h"
#include "amdroid/mouse.h"

extern const app_t app_store;
extern void gui_damage_all(void);          /* repaint the whole screen (gui.c) */

#define ROW_H  60
#define SEC_H  40
#define BTN_W  116
#define BTN_H  40

static pkg_meta_t g_inst[PKG_MAX_APPS];
static int        g_ninst;
static char       g_avail[PKG_MAX_APPS][16];
static int        g_navail;
static int        g_modal;                 /* 0 none, 1 install-confirm */
static pkg_meta_t g_pend;                  /* package shown in the confirm sheet */
static pkg_report_t g_report;              /* its structural/version/security report */
static char       g_pend_file[16];         /* the .adp file to install */
static char       g_status[72];
static int        g_sel;                   /* selected row: 0..ninst-1 installed, then available */

static int total_rows(void){ return g_ninst + g_navail; }

static void refresh(void){
    g_ninst  = pkg_list(g_inst, PKG_MAX_APPS);
    g_navail = pkg_available(g_avail, PKG_MAX_APPS);
    int n = total_rows();
    if(g_sel >= n) g_sel = n ? n - 1 : 0;
    if(g_sel < 0) g_sel = 0;
}

static void s_open(void){
    g_modal = 0;
    g_status[0] = 0;
    refresh();
}

/* ---- drawing helpers ---- */

static void button(rect_t r, const char *label, u32 bg, u32 fg){
    ui_rrect(r.x, r.y, r.w, r.h, 8, bg);
    u32 tw = ui_text_w(label, 2);
    ui_text(r.x + (r.w - tw) / 2, r.y + (r.h - UI_CH) / 2, label, fg, 2);
}

/* geometry shared by draw and click */
static rect_t inst_row(rect_t b, int i){
    return (rect_t){ b.x, b.y + 48 + SEC_H + (u32)i * ROW_H, b.w, ROW_H };
}
static u32 avail_top(rect_t b){
    return b.y + 48 + SEC_H + (u32)(g_ninst ? g_ninst : 1) * ROW_H + 16;
}
static rect_t avail_row(rect_t b, int i){
    return (rect_t){ b.x, avail_top(b) + SEC_H + (u32)i * ROW_H, b.w, ROW_H };
}
static rect_t row_btn(rect_t row){
    return (rect_t){ row.x + row.w - BTN_W - 8, row.y + (ROW_H - BTN_H) / 2, BTN_W, BTN_H };
}

/* the install-confirmation sheet, centred in the body */
static rect_t sheet_rect(rect_t b){
    u32 w = b.w - 80; if(w > 640) w = 640;
    u32 h = 400; if(h > b.h - 40) h = b.h - 40;
    return (rect_t){ b.x + (b.w - w) / 2, b.y + (b.h - h) / 2, w, h };
}
static rect_t sheet_cancel(rect_t s){ return (rect_t){ s.x + s.w - 2 * (BTN_W + 16), s.y + s.h - BTN_H - 20, BTN_W, BTN_H }; }
static rect_t sheet_install(rect_t s){ return (rect_t){ s.x + s.w - (BTN_W + 16), s.y + s.h - BTN_H - 20, BTN_W, BTN_H }; }

static int perms_dangerous(const char *p){
    const char *d[] = { "storage", "network", "system" };
    for(int i = 0; i < 3; i++){
        for(const char *s = p; *s; s++){
            const char *a = s, *b = d[i];
            while(*a && *b && *a == *b){ a++; b++; }
            if(!*b) return 1;
        }
    }
    return 0;
}

/* draw a block of text, wrapped on newlines, returning the y below it */
static u32 draw_lines(u32 x, u32 y, const char *s, u32 c){
    char line[96]; int n = 0;
    for(;; s++){
        if(*s == '\n' || *s == 0){
            line[n] = 0;
            if(n) ui_text(x, y, line, c, 1);
            y += 20; n = 0;
            if(*s == 0) break;
        } else if(n < (int)sizeof line - 1) line[n++] = *s;
    }
    return y;
}

static void draw_sheet(rect_t b){
    ui_fill(b, 0x0A0E12);                              /* dim the body */
    rect_t s = sheet_rect(b);
    ui_rrect(s.x, s.y, s.w, s.h, 14, C_CARD);
    u32 x = s.x + 28, y = s.y + 24;

    int dng = perms_dangerous(g_pend.perms);
    ui_text(x, y, g_report.blocking ? "Install blocked" : "Install app?",
            g_report.blocking ? C_WARN : C_MUTED, UI_SCALE);
    y += UI_CH + 10;
    ui_icon(g_pend.icon, x, y + 4, ui_accent());
    ui_text(x + 48, y, g_pend.name[0] ? g_pend.name : g_pend_file, C_TEXT, UI_SCALE);
    y += UI_CH + 6;
    char line[96] = "v"; ui_cat(line, g_pend.version);
    if(g_pend.author[0]){ ui_cat(line, "  -  "); ui_cat(line, g_pend.author); }
    ui_text(x, y, line, C_DIM, 1); y += 24;

    char rq[48] = "Requires: ";
    ui_cat(rq, g_pend.requires[0] ? g_pend.requires : "(unset)");
    ui_text(x, y, rq, g_report.version_ok ? C_GOOD : C_WARN, 1); y += 22;

    ui_text(x, y, "Permissions", C_DIM, 1); y += 18;
    ui_text(x, y, g_pend.perms[0] ? g_pend.perms : "none", dng ? C_WARN : C_MUTED, 1);
    if(dng) ui_text(x + ui_text_w(g_pend.perms, 1) + 12, y, "(dangerous)", C_WARN, 1);
    y += 24;

    ui_text(x, y, "Security scan", C_DIM, 1); y += 18;
    if(g_report.error[0] && g_report.blocking){
        y = draw_lines(x, y, g_report.error, C_WARN);
    }
    if(g_report.nfindings){
        y = draw_lines(x, y, g_report.findings, C_WARN);
    } else if(!g_report.error[0]){
        ui_text(x, y, g_report.dangerous ? "OK (requests dangerous permissions)" : "OK - no issues found",
                C_GOOD, 1);
        y += 20;
    }

    button(sheet_cancel(s), "Cancel", C_HOVER, C_TEXT);
    if(g_report.blocking)
        button(sheet_install(s), "Blocked", 0x3A2630, C_DIM);
    else
        button(sheet_install(s), "Install", ui_accent(), 0x0A0E12);
}

static void s_draw(rect_t b, int focused){
    (void)focused;
    ui_text(b.x, b.y, "App Store", ui_accent(), 4);

    /* Installed section */
    char head[40] = "Installed";
    ui_text(b.x, b.y + 48, head, C_TEXT, UI_SCALE);
    if(g_ninst == 0){
        ui_text(b.x, b.y + 48 + SEC_H + 8, "No apps installed yet.", C_DIM, UI_SCALE);
    }
    for(int i = 0; i < g_ninst; i++){
        rect_t r = inst_row(b, i);
        int sel = !g_modal && g_sel == i;
        ui_rrect(r.x, r.y + 4, r.w, ROW_H - 8, 10, sel ? C_CARD_HI : C_CARD);
        if(sel) ui_fill((rect_t){ r.x, r.y + 8, 4, ROW_H - 16 }, ui_accent());
        ui_icon(g_inst[i].icon, r.x + 14, r.y + (ROW_H - 32) / 2, ui_accent());
        ui_text(r.x + 60, r.y + 10, g_inst[i].name, C_TEXT, UI_SCALE);
        char sub[64] = "v"; ui_cat(sub, g_inst[i].version);
        ui_cat(sub, "   Tap to open");
        ui_text(r.x + 60, r.y + 36, sub, C_DIM, 1);
        button(row_btn(r), "Remove", C_HOVER, C_WARN);
    }

    /* Available section */
    u32 ay = avail_top(b);
    ui_text(b.x, ay, "Available on disk", C_TEXT, UI_SCALE);
    if(g_navail == 0){
        ui_text(b.x, ay + SEC_H + 8, "No .adp packages found on the disk.", C_DIM, UI_SCALE);
    }
    for(int i = 0; i < g_navail; i++){
        rect_t r = avail_row(b, i);
        int sel = !g_modal && g_sel == g_ninst + i;
        ui_rrect(r.x, r.y + 4, r.w, ROW_H - 8, 10, sel ? C_CARD_HI : 0x1A2228);
        if(sel) ui_fill((rect_t){ r.x, r.y + 8, 4, ROW_H - 16 }, ui_accent());
        ui_icon(UI_ICO_STORE, r.x + 14, r.y + (ROW_H - 32) / 2, C_MUTED);
        ui_text(r.x + 60, r.y + 18, g_avail[i], C_TEXT, UI_SCALE);
        button(row_btn(r), "Install", ui_accent(), 0x0A0E12);
    }

    if(g_status[0]) ui_text(b.x, b.y + b.h - 22, g_status, C_GOOD, 1);

    if(g_modal) draw_sheet(b);
}

/* ---- actions ---- */

static void do_install(void){
    if(g_report.blocking){
        ui_copy(g_status, "Blocked: ", sizeof g_status);
        ui_cat(g_status, g_report.error[0] ? g_report.error : "failed the security scan");
        g_modal = 0;
        return;
    }
    pkg_meta_t out;
    char err[104] = "";
    if(pkg_install_file(g_pend_file, &out, err, sizeof err) == 0){
        ui_copy(g_status, "Installed ", sizeof g_status);
        ui_cat(g_status, out.name);
        ui_notify(ui_app_index(&app_store), "App installed", out.name);
    } else {
        ui_copy(g_status, "Install failed: ", sizeof g_status);
        ui_cat(g_status, err[0] ? err : "unknown error");
    }
    g_modal = 0;
    refresh();
}

static void launch(const char *id){
    char path[64];
    if(pkg_entry_path(id, path, sizeof path) != 0){
        ui_copy(g_status, "Cannot find the app's program", sizeof g_status);
        return;
    }
    mouse_event_t ev;
    while(mouse_poll(&ev)) { }      /* swallow the launching click so the app does not exit at once */
    fuss_run_file(path);            /* runs full screen until the app exits */
    while(mouse_poll(&ev)) { }      /* and the exiting click, so the desktop does not react */
    gui_damage_all();               /* repaint the desktop underneath */
    ui_copy(g_status, "", sizeof g_status);
}

static void open_confirm(const char *file){
    u32 sz = 0;
    char path[16];
    ui_copy(path, file, sizeof path);
    /* read the package header to show in the sheet */
    extern int fat32_read_path(const char *p, void *buf, u32 max, u32 *out);
    if(fat32_read_path(path, 0, 0, &sz) != 0) return;
    char *buf = 0;
    extern void *kmalloc(unsigned long); extern void kfree(void *);
    buf = kmalloc(sz + 1);
    if(!buf) return;
    u32 got = 0;
    if(fat32_read_path(path, buf, sz, &got) == 0){
        buf[got] = 0;
        pkg_check(buf, got, &g_pend, &g_report);   /* fills meta + report even on failure */
        ui_copy(g_pend_file, file, sizeof g_pend_file);
        g_modal = 1;
    }
    kfree(buf);
}

/* ---- input ---- */

static void s_click(rect_t b, int x, int y){
    if(g_modal){
        rect_t s = sheet_rect(b);
        if(ui_contains(sheet_install(s), x, y)){ do_install(); return; }
        if(ui_contains(sheet_cancel(s), x, y)){ g_modal = 0; return; }
        if(!ui_contains(s, x, y)) g_modal = 0;      /* tap outside dismisses */
        return;
    }
    for(int i = 0; i < g_ninst; i++){
        rect_t r = inst_row(b, i);
        if(ui_contains(row_btn(r), x, y)){ pkg_remove(g_inst[i].id); ui_copy(g_status, "Removed", sizeof g_status); refresh(); return; }
        if(ui_contains(r, x, y)){ launch(g_inst[i].id); return; }
    }
    for(int i = 0; i < g_navail; i++){
        rect_t r = avail_row(b, i);
        if(ui_contains(r, x, y)){ open_confirm(g_avail[i]); return; }
    }
}

static void activate(void){
    if(g_sel < 0) return;
    if(g_sel < g_ninst) launch(g_inst[g_sel].id);
    else { int ai = g_sel - g_ninst; if(ai < g_navail) open_confirm(g_avail[ai]); }
}

static int s_key(int c){
    if(g_modal){
        if(c == 27){ g_modal = 0; return 1; }       /* Esc closes the sheet */
        if(c == '\n' || c == '\r'){ do_install(); return 1; }
        return 1;                                    /* modal swallows keys */
    }
    int n = total_rows();
    if(c == KEY_UP){ if(g_sel > 0) g_sel--; return 1; }
    if(c == KEY_DOWN){ if(g_sel < n - 1) g_sel++; return 1; }
    if(c == '\n' || c == '\r'){ activate(); return 1; }
    if((c == 'x' || c == 'X' || c == 0x7f) && g_sel < g_ninst && g_ninst > 0){
        pkg_remove(g_inst[g_sel].id); ui_copy(g_status, "Removed", sizeof g_status); refresh(); return 1;
    }
    if(c == 'r' || c == 'R'){ refresh(); return 1; }
    return 0;
}

const app_t app_store = {
    .name = "App Store",
    .icon = UI_ICO_STORE,
    .open = s_open,
    .close = NULL,
    .draw = s_draw,
    .key = s_key,
    .click = s_click,
};
