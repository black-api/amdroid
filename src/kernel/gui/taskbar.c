/* Taskbar (Start, Search, pinned and open apps, Terminal, clock) and the Start menu. */
#include "gui_internal.h"
#include "amdroid/sched.h"
#include "amdroid/pmm.h"
#include "amdroid/timer.h"
#include "amdroid/keyboard.h"
#include "amdroid/fb.h"
#include "amdroid/pkg.h"

#define BAR_H       48
#define BAR_MARGIN  12                 /* gap between the taskbar and the screen bottom */
#define BAR_ITEM    52                 /* width of one taskbar slot */
#define BAR_PAD     14
#define BAR_STATS_W 118                /* live CPU / RAM block inside the pill */
#define BAR_CLOCK_W 108                /* the clock lives inside the pill too */

/* Every app always has its own slot on the taskbar, pinned or not, so
 * opening and closing an app never shifts the bar. */

int tb_items(item_t *it){
    int n = 0;
    it[n++] = (item_t){ IT_START, 0 };
    it[n++] = (item_t){ IT_SEARCH, 0 };
    for(int a = 0; a < ui_app_count(); a++) it[n++] = (item_t){ IT_APP, a };
    it[n++] = (item_t){ IT_TERM, 0 };
    return n;
}

rect_t tb_bar(int n, u32 sw, u32 sh){
    u32 w = (u32)n * BAR_ITEM + 2 * BAR_PAD + BAR_STATS_W + BAR_CLOCK_W;
    return (rect_t){ (sw - w) / 2, sh - BAR_MARGIN - BAR_H, w, BAR_H };
}
static rect_t tb_item(rect_t bar, int i){
    return (rect_t){ bar.x + BAR_PAD + (u32)i * BAR_ITEM, bar.y + 6, BAR_ITEM - 8, BAR_H - 12 };
}
int tb_hit(rect_t bar, int n, int x, int y){
    for(int i = 0; i < n; i++)
        if(ui_contains(tb_item(bar, i), x, y)) return i;
    return -1;
}

/* ---- live system stats (real measurements, sampled twice a second) ----
 * CPU comes from the scheduler's TSC accounting (busy = not halted-idle),
 * RAM from the physical page allocator. */
#define STATS_TICKS 50               /* 0.5 s at 100 Hz */
static u64 st_last_tsc = 0, st_last_idle = 0, st_last_at = 0;
static int st_sampled = 0;
static u32 st_cpu = 0;               /* tenths of a percent */
static u32 st_ram_pct = 0;           /* whole percent */
static char st_clock[8] = "";       /* hh:mm, read from the RTC with the rest */
static void tb_stats(u64 now){
    if(st_sampled && now - st_last_at < STATS_TICKS) return;
    ui_clock(st_clock);
    proc_info_t pi[4];
    u64 idle = 0;
    proc_info(pi, 4, &idle);
    u64 tsc = sched_tsc();
    if(st_sampled && tsc > st_last_tsc){
        u64 dt = tsc - st_last_tsc;
        u64 di = idle >= st_last_idle ? idle - st_last_idle : 0;
        st_cpu = di >= dt ? 0 : (u32)((dt - di) * 1000 / dt);
        if(st_cpu > 1000) st_cpu = 1000;
    }
    u64 tot = pmm_total_pages(), free = pmm_free_pages();
    st_ram_pct = tot ? (u32)((tot > free ? tot - free : 0) * 100 / tot) : 0;
    st_last_tsc = tsc;
    st_last_idle = idle;
    st_last_at = now;
    st_sampled = 1;
}

/* The bar is redrawn when anything it shows changes: the numbers, the clock, the icon under
 * the pointer, which apps are open and which one has the focus. The label over an icon is
 * part of it. */
#define TIP_H 44
void tb_damage(int n, rect_t bar, int hover, int hover_amt, int top, int start_open, u64 now){
    static int sh_hover = -1, sh_amt, sh_top = -1, sh_start, sh_open;
    static u32 sh_cpu, sh_ram;
    static char sh_clock[8];
    tb_stats(now);
    int open = 0;
    for(int a = 0; a < ui_app_count(); a++) if(win_is_open(a)) open |= 1 << a;
    int icons = hover == sh_hover && hover_amt == sh_amt && top == sh_top && start_open == sh_start && open == sh_open;
    int numbers = st_cpu == sh_cpu && st_ram_pct == sh_ram && ui_streq(st_clock, sh_clock);
    if(icons && numbers) return;
    if(icons){
        /* only the numbers moved: they sit at the right end, after the icons */
        int sx = (int)(bar.x + BAR_PAD + (u32)n * BAR_ITEM);
        gui_damage(sx, (int)bar.y + 2, (int)(bar.x + bar.w) - sx - 16, (int)bar.h - 4);
    } else gui_damage((int)bar.x - 48, (int)bar.y - TIP_H, (int)bar.w + 96, (int)bar.h + TIP_H);
    sh_hover = hover; sh_amt = hover_amt; sh_top = top; sh_start = start_open; sh_open = open;
    sh_cpu = st_cpu; sh_ram = st_ram_pct;
    ui_copy(sh_clock, st_clock, sizeof sh_clock);
}

void tb_draw(const item_t *it, int n, rect_t bar, int hover, int hover_amt, int top, int start_open){
    if(!fb_clip_hits((i32)bar.x - 48, (i32)bar.y - TIP_H, (i32)bar.w + 96, (i32)bar.h + TIP_H)) return;
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
            else if(!win_is_open(it[i].app)) col = C_DIM;   /* closed apps sit dim in their fixed slot */
        }
        ui_icon(ico, r.x + (r.w - 32) / 2, r.y + 2, col);
        if(it[i].kind == IT_APP && win_is_open(it[i].app)){
            int focused = it[i].app == top;
            u32 dot_w = focused ? 18 : 6;
            ui_rrect(r.x + (r.w - dot_w) / 2, r.y + r.h - 5, dot_w, 3, 1,
                     focused ? ui_accent() : C_MUTED);
        }
    }
    /* live CPU / RAM, then the clock, both inside the pill so they never
     * float over a window */
    {
        u32 sx = bar.x + BAR_PAD + (u32)n * BAR_ITEM;
        char cpu[16] = "CPU ", pct[12], num[12];
        ui_num(st_cpu / 10, num); ui_cat(cpu, num); ui_cat(cpu, ".");
        ui_num(st_cpu % 10, pct); ui_cat(cpu, pct); ui_cat(cpu, "%");
        char ram[16] = "RAM ", rp[12];
        ui_num(st_ram_pct, rp); ui_cat(ram, rp); ui_cat(ram, "%");
        ui_fill((rect_t){ sx, bar.y + 10, 1, bar.h - 20 }, C_SEP);
        u32 fill = (u32)(((u64)bar.h * st_cpu) / 1000);
        if(fill > bar.h - 6) fill = bar.h - 6;
        ui_fill((rect_t){ sx + 7, bar.y + 3 + (bar.h - 6) - fill, 3, fill }, ui_accent());
        ui_text(sx + 16, bar.y + 7, cpu, C_TEXT, 1);
        ui_text(sx + 16, bar.y + 25, ram, C_MUTED, 1);
        u32 cw = ui_text_w(st_clock, UI_SCALE);
        int ty = (int)bar.y + ((int)bar.h - (int)UI_CH) / 2;
        ui_text(bar.x + bar.w - cw - 20, (u32)ty, st_clock, C_MUTED, UI_SCALE);
    }
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

/* ---- Start menu: a search box, then the apps, Terminal and Sign out that match it ---- */
#define MENU_W    380
#define MENU_ROW  44
#define MENU_TOP  68                   /* the search box */
#define MENU_SEP  16                   /* the gap (with a line) before Terminal and Sign out */
#define MENU_PKG_MAX 16
#define MENU_FIRST_PKG (UI_MAX_APPS + 2)          /* ids at or above this are installed apps */
#define MENU_ENTRIES (UI_MAX_APPS + 2 + MENU_PKG_MAX)
#define QUERY_MAX 20

static char query[QUERY_MAX + 1];
static int qlen = 0;
static int msel = 0;                   /* the entry Enter opens: an index into the matches */

/* installed .adp apps, refreshed when the menu opens, so they are searchable too */
static pkg_meta_t g_pkgs[MENU_PKG_MAX];
static int g_npkg = 0;

/* The installed-app id for a menu id, or NULL if it is a built-in/Terminal/Sign out. */
const char *tb_menu_pkg(int id){
    if(id >= MENU_FIRST_PKG && id - MENU_FIRST_PKG < g_npkg) return g_pkgs[id - MENU_FIRST_PKG].id;
    return 0;
}

static const char *entry_name(int id){
    if(id == UI_MAX_APPS) return "Terminal";
    if(id == UI_MAX_APPS + 1) return "Sign out";
    if(id >= MENU_FIRST_PKG) return g_pkgs[id - MENU_FIRST_PKG].name;
    return ui_apps[id]->name;
}
static int menu_icon(int id){
    if(id == UI_MAX_APPS) return UI_ICO_TERM;
    if(id >= MENU_FIRST_PKG) return g_pkgs[id - MENU_FIRST_PKG].icon;
    return ui_apps[id]->icon;
}

/* 1 when the search text appears in name, ignoring case. */
static int matches(const char *name){
    if(!qlen) return 1;
    for(const char *s = name; *s; s++){
        int k = 0;
        while(k < qlen && s[k] && (s[k] | 32) == (query[k] | 32)) k++;
        if(k == qlen) return 1;
    }
    return 0;
}

/* The entries that match the search, in menu order. */
static int found(int *ids){
    int n = 0, apps = ui_app_count();
    for(int a = 0; a < apps; a++) if(matches(entry_name(a))) ids[n++] = a;
    for(int k = 0; k < g_npkg; k++)
        if(matches(g_pkgs[k].name) || matches(g_pkgs[k].id)) ids[n++] = MENU_FIRST_PKG + k;
    for(int id = UI_MAX_APPS; id <= UI_MAX_APPS + 1; id++) if(matches(entry_name(id))) ids[n++] = id;
    return n;
}

void tb_menu_reset(void){
    qlen = 0; query[0] = 0; msel = 0;
    g_npkg = pkg_list(g_pkgs, MENU_PKG_MAX);       /* so installed apps are searchable */
}

rect_t tb_menu(rect_t bar, u32 sw){
    u32 h = MENU_TOP + (u32)(ui_app_count() + g_npkg + 2) * MENU_ROW + MENU_SEP + 12;
    return (rect_t){ (sw - MENU_W) / 2, bar.y - 16 - h, MENU_W, h };
}
/* Row i of the matches. With no search the list is whole, and a gap sets Terminal and
 * Sign out apart. */
static rect_t menu_row(rect_t m, int i){
    u32 extra = !qlen && i >= ui_app_count() + g_npkg ? MENU_SEP : 0;
    return (rect_t){ m.x + 10, m.y + MENU_TOP + (u32)i * MENU_ROW + extra, m.w - 20, MENU_ROW - 4 };
}
static int row_at(rect_t m, int x, int y){
    int ids[MENU_ENTRIES], n = found(ids);
    for(int i = 0; i < n; i++) if(ui_contains(menu_row(m, i), x, y)) return i;
    return -1;
}
int tb_menu_hit(rect_t m, int x, int y){
    int ids[MENU_ENTRIES];
    found(ids);
    int i = row_at(m, x, y);
    return i < 0 ? -1 : ids[i];
}

int tb_menu_key(int c){
    int ids[MENU_ENTRIES], n = found(ids);
    if(c == 27) return TB_KEY_CLOSE;
    if(c == '\n') return n ? ids[msel < n ? msel : 0] : TB_KEY_USED;
    if(c == KEY_UP){ if(msel > 0) msel--; return TB_KEY_USED; }
    if(c == KEY_DOWN){ if(msel < n - 1) msel++; return TB_KEY_USED; }
    if(c == '\b'){ if(qlen) query[--qlen] = 0; msel = 0; return TB_KEY_USED; }
    if(c >= 32 && c <= 126){
        if(qlen < QUERY_MAX){ query[qlen++] = (char)c; query[qlen] = 0; }
        msel = 0;
        return TB_KEY_USED;
    }
    return TB_KEY_UNUSED;
}

int tb_menu_slide(u64 now, u64 opened_at){
    if(!ui_anim_on || now - opened_at >= 12) return 0;
    return (255 - ui_ease((int)((now - opened_at) * 255 / 12))) * 24 / 255;
}

void tb_menu_damage(rect_t bar, u32 sw, int open, int dy, int mx, int my){
    static int sh_open, sh_dy, sh_hot = -1, sh_sel, sh_qlen;
    rect_t m = tb_menu(bar, sw);
    int hot = open ? row_at(m, mx, my) : -1;
    if(open == sh_open && dy == sh_dy && hot == sh_hot && msel == sh_sel && qlen == sh_qlen) return;
    gui_damage((int)m.x, (int)m.y, (int)m.w + 6, (int)m.h + 10 + 24);    /* with its shadow and its slide */
    sh_open = open; sh_dy = dy; sh_hot = hot; sh_sel = msel; sh_qlen = qlen;
}

void tb_menu_draw(rect_t bar, u32 sw, int dy, int mx, int my){
    rect_t m = tb_menu(bar, sw);
    int hot = row_at(m, mx, my);              /* (before the slide offset: the row under the pointer) */
    m.y += (u32)dy;
    if(!fb_clip_hits((i32)m.x, (i32)m.y, (i32)m.w + 6, (i32)m.h + 10)) return;
    ui_rrect(m.x + 6, m.y + 10, m.w, m.h, 16, C_SHADOW);
    ui_rrect(m.x, m.y, m.w, m.h, 16, C_MENU);

    /* the search box: what was typed, with a caret */
    rect_t sb = { m.x + 14, m.y + 14, m.w - 28, 44 };
    ui_rrect(sb.x, sb.y, sb.w, sb.h, 10, qlen ? ui_accent() : C_BAR_EDGE);
    ui_rrect(sb.x + 2, sb.y + 2, sb.w - 4, sb.h - 4, 9, C_CARD);
    ui_icon(UI_ICO_SEARCH, sb.x + 8, sb.y + 6, C_MUTED);
    ui_text(sb.x + 50, sb.y + 6, qlen ? query : "Type to search", qlen ? C_TEXT : C_DIM, UI_SCALE);
    if(qlen) ui_fill((rect_t){ sb.x + 52 + (u32)qlen * UI_CW, sb.y + 10, 2, sb.h - 20 }, ui_accent());

    int ids[MENU_ENTRIES], n = found(ids);
    if(!n) ui_text(m.x + 24, m.y + MENU_TOP + 8, "Nothing matches", C_DIM, UI_SCALE);
    if(!qlen){
        /* the line between the apps and Terminal / Sign out */
        u32 sep_y = m.y + MENU_TOP + (u32)(ui_app_count() + g_npkg) * MENU_ROW + MENU_SEP / 2 - 2;
        ui_fill((rect_t){ m.x + 20, sep_y, m.w - 40, 1 }, C_SEP);
    }
    for(int i = 0; i < n; i++){
        rect_t r = menu_row(m, i);
        int id = ids[i];
        /* the pointer's row, and with a search the one Enter opens */
        if(i == hot || (qlen && i == msel)){
            ui_rrect(r.x, r.y, r.w, r.h, 10, i == hot ? C_HOVER : C_CARD);
            ui_rrect(r.x, r.y + 8, 3, r.h - 16, 1, ui_accent());
        }
        u32 ty = r.y + (r.h - UI_CH) / 2;
        if(id == UI_MAX_APPS + 1){
            ui_text(r.x + 56, ty, entry_name(id), C_WARN, UI_SCALE);
        } else {
            ui_icon(menu_icon(id), r.x + 12, r.y + (r.h - 32) / 2, C_TEXT);
            ui_text(r.x + 56, ty, entry_name(id), C_TEXT, UI_SCALE);
        }
    }
}
