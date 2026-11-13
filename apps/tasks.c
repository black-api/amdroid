/* Tasks: a task manager with live numbers.
 *
 * What it measures (all real):
 *  - CPU: the scheduler counts CPU cycles (TSC) for every process at each switch, and the
 *    time spent halted as idle. The GUI apps run inside the kernel (pid 0), so the GUI
 *    also times each app's own code; the kernel row is what is left.
 *  - Memory: a process's resident pages come from its page tables. A GUI app's memory is
 *    the kernel heap blocks it allocated (they are tagged with the app). The kernel row is
 *    the rest of the used physical memory.
 *  - Group RSS / Group CPU: the row plus everything under it (the kernel holds all of it;
 *    a process holds the processes it started).
 *
 * Rows turn green when a task starts, yellow when its numbers change a lot, and red for a
 * moment when it ends. Graphs show the last minute of CPU and memory. */
#include "amdroid/ui.h"
#include "amdroid/sched.h"
#include "amdroid/keyboard.h"
#include "amdroid/heap.h"
#include "amdroid/pmm.h"
#include "amdroid/fat32.h"
#include "amdroid/elf.h"
#include "amdroid/timer.h"

#define SAMPLE_TICKS 50            /* a sample every 0.5 s */
#define HIST         120           /* one minute of samples */
#define ROWS_MAX     (MAX_PROCS + UI_MAX_APPS + 1)
#define GHOSTS_MAX   8
#define MARK_TICKS   150           /* how long the start/end colours last */

#define COL_START  0x7FCB6E        /* the legend's colours */
#define COL_CHANGE 0xF2D45C
#define COL_END    0xE8716F
#define G_BG       0x0B0D0E        /* graphs */
#define G_GRID     0x2C3A34
#define G_FILL     0x1F4D3E
#define G_LINE     0x45D6A0
#define M_FILL     0x1F3A55
#define M_LINE     0x5AB4F0

extern const app_t app_tasks;

enum { K_KERNEL, K_APP, K_PROC };

typedef struct {
    int kind, id;                  /* id: app index or pid */
    u32 serial;
    int parent;                    /* index of the parent row, or -1 */
    char name[32];
    const char *state;
    u64 cycles;                    /* total so far */
    u64 rss;                       /* bytes */
    u32 cpu;                       /* tenths of a percent, last interval */
    u64 grss; u32 gcpu;            /* with everything under it */
    u64 born;                      /* when it first showed up */
    u64 changed;                   /* when its numbers last jumped */
} row_t;

static row_t rows[ROWS_MAX], prev[ROWS_MAX];
static int nrows = 0, nprev = 0;
static row_t ghosts[GHOSTS_MAX];   /* ended: shown red for a moment */
static u64 ghost_at[GHOSTS_MAX];
static int nghosts = 0;

static u64 last_sample = 0, last_tsc = 0, last_idle = 0;
static int sampled = 0;
static u16 cpu_hist[HIST];                    /* tenths of a percent */
static u64 mem_hist_b[HIST];                  /* bytes in use */
static u16 mem_hist[HIST];                    /* tenths of the real physical memory */
static int nhist = 0;
static u32 cpu_now = 0;
static u64 mem_used = 0, mem_total = 0;

static int sort_col = 0, sort_desc = 0;      /* 0 pid, 1 name, 2 rss, 3 group rss, 4 cpu, 5 group cpu */
static int sel_kind = K_KERNEL, sel_id = 0;  /* the selected row, by identity (survives re-sorting) */
static int top = 0;
static int picking = 0;                       /* the "New task" list is open */
static char progs[16][16];
static int nprogs = 0;
static rect_t last_body;

/* ---- sampling ---- */

static int same_task(const row_t *a, const row_t *b){
    return a->kind == b->kind && a->id == b->id && a->serial == b->serial;
}

static void sample(u64 now){
    proc_info_t pi[MAX_PROCS];
    u64 idle = 0;
    int np = proc_info(pi, MAX_PROCS, &idle);
    u64 tsc = sched_tsc();
    u64 dt = sampled ? tsc - last_tsc : 0;

    u64 owners[UI_MAX_APPS + 1], hused = 0, htotal = 0;
    heap_stats(&hused, &htotal, owners, UI_MAX_APPS + 1);
    /* the kernel's heap is one fixed block reserved at boot (counted as used pages from the
       start): count only the part of it that is really allocated */
    u64 phys_used = (pmm_total_pages() - pmm_free_pages()) * 4096;
    if(phys_used > htotal) phys_used = phys_used - htotal + hused;

    nprev = nrows;
    for(int i = 0; i < nrows; i++) prev[i] = rows[i];
    nrows = 0;

    /* the kernel, then the GUI apps inside it, then the processes */
    row_t *k = &rows[nrows++];
    *k = (row_t){ .kind = K_KERNEL, .id = 0, .parent = -1, .state = "running" };
    ui_copy(k->name, "kernel", sizeof k->name);
    u64 apps_cyc = 0, apps_mem = 0, user_mem = 0;
    for(int a = 0; a < ui_app_count(); a++){
        if(!ui_app_is_open(a)) continue;
        row_t *r = &rows[nrows++];
        *r = (row_t){ .kind = K_APP, .id = a, .parent = 0, .state = "app" };
        ui_copy(r->name, ui_apps[a]->name, sizeof r->name);
        r->cycles = ui_app_cycles(a);
        r->rss = owners[a + 1];
        apps_cyc += r->cycles;
        apps_mem += r->rss;
    }
    for(int i = 0; i < np; i++){
        if(pi[i].pid == 0){ k->cycles = pi[i].cycles; continue; }
        row_t *r = &rows[nrows++];
        *r = (row_t){ .kind = K_PROC, .id = pi[i].pid, .serial = pi[i].serial, .parent = -1, .state = pi[i].state };
        /* the name: the program file without its folder */
        const char *nm = pi[i].name, *base = nm;
        for(const char *q = nm; *q; q++) if(*q == '/') base = q + 1;
        ui_copy(r->name, base, sizeof r->name);
        r->cycles = pi[i].cycles;
        r->rss = pi[i].pages * 4096;
        r->parent = pi[i].ppid;            /* a pid for now; made a row index below */
        user_mem += r->rss;
    }
    /* the apps' time and memory are counted inside the kernel's: take them out */
    k->cycles = k->cycles > apps_cyc ? k->cycles - apps_cyc : 0;
    u64 rest = apps_mem + user_mem;
    k->rss = phys_used > rest ? phys_used - rest : 0;

    /* parents as row indices: a process started by pid 0 belongs to the kernel */
    for(int i = 0; i < nrows; i++){
        if(rows[i].kind != K_PROC) continue;
        int pp = rows[i].parent, found = 0;
        for(int j = 0; j < nrows; j++)
            if(rows[j].kind == K_PROC && rows[j].id == pp && j != i){ rows[i].parent = j; found = 1; break; }
        if(!found) rows[i].parent = 0;
    }

    /* CPU over the last interval, and the colour marks */
    for(int i = 0; i < nrows; i++){
        row_t *r = &rows[i];
        r->born = now;
        int seen = -1;
        for(int j = 0; j < nprev; j++) if(same_task(&prev[j], r)){ seen = j; break; }
        if(seen >= 0){
            r->born = prev[seen].born;
            r->changed = prev[seen].changed;
            u64 dc = r->cycles >= prev[seen].cycles ? r->cycles - prev[seen].cycles : 0;
            r->cpu = dt ? (u32)(dc * 1000 / dt) : 0;
            if(r->cpu > 1000) r->cpu = 1000;
            /* yellow when the CPU moves by 5 points or the memory by 256 KB */
            int dcpu = (int)r->cpu - (int)prev[seen].cpu;
            u64 dm = r->rss > prev[seen].rss ? r->rss - prev[seen].rss : prev[seen].rss - r->rss;
            if(sampled && (dcpu > 50 || dcpu < -50 || dm >= 256 * 1024)) r->changed = now;
        } else if(!sampled) r->born = 0;               /* there when the window opened: not "new" */
    }
    /* the ones that ended stay a moment, in red */
    for(int j = 0; j < nprev; j++){
        int alive = 0;
        for(int i = 0; i < nrows; i++) if(same_task(&prev[j], &rows[i])){ alive = 1; break; }
        if(alive) continue;
        if(nghosts == GHOSTS_MAX){ for(int g = 1; g < GHOSTS_MAX; g++){ ghosts[g - 1] = ghosts[g]; ghost_at[g - 1] = ghost_at[g]; } nghosts--; }
        ghosts[nghosts] = prev[j];
        ghosts[nghosts].cpu = 0;
        ghosts[nghosts].parent = -1;
        ghost_at[nghosts++] = now;
    }
    int g2 = 0;
    for(int g = 0; g < nghosts; g++) if(now - ghost_at[g] < MARK_TICKS){ ghosts[g2] = ghosts[g]; ghost_at[g2++] = ghost_at[g]; }
    nghosts = g2;

    /* groups: each row adds itself to every row above it */
    for(int i = 0; i < nrows; i++){ rows[i].grss = 0; rows[i].gcpu = 0; }
    for(int i = 0; i < nrows; i++){
        int guard = 0;
        for(int j = i; j >= 0 && guard < ROWS_MAX; j = rows[j].parent, guard++){
            rows[j].grss += rows[i].rss;
            rows[j].gcpu += rows[i].cpu;
        }
    }

    /* the whole machine: busy = not idle */
    if(dt){
        u64 di = idle - last_idle;
        cpu_now = di >= dt ? 0 : (u32)((dt - di) * 1000 / dt);
    }
    mem_used = phys_used;
    mem_total = pmm_total_pages() * 4096;
    if(sampled){
        if(nhist == HIST){ for(int i = 1; i < HIST; i++){ cpu_hist[i - 1] = cpu_hist[i]; mem_hist_b[i - 1] = mem_hist_b[i]; } nhist--; }
        cpu_hist[nhist] = (u16)cpu_now;
        mem_hist_b[nhist] = mem_used;
        nhist++;
        /* the memory graph uses the machine's real memory as a fixed scale, so
           the line never jumps when usage grows (an auto-zoom rescaling the
           past looked like a glitch) */
        for(int i = 0; i < nhist; i++) mem_hist[i] = (u16)(mem_total ? mem_hist_b[i] * 1000 / mem_total : 0);
    }
    last_tsc = tsc;
    last_idle = idle;
    last_sample = now;
    sampled = 1;
}

/* ---- the list: the rows in display order (sorted), ghosts at the end ---- */

static u64 key_of(const row_t *r, int col){
    switch(col){
        case 0: return (u64)(r->kind == K_PROC ? r->id * 100 + 50 : r->kind == K_APP ? r->id + 1 : 0);
        case 2: return r->rss;
        case 3: return r->grss;
        case 4: return r->cpu;
        case 5: return r->gcpu;
    }
    return 0;
}
static int name_cmp(const char *a, const char *b){
    while(*a && *b){
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if(x != y) return x - y;
        a++; b++;
    }
    return *a - *b;
}
static int order[ROWS_MAX + GHOSTS_MAX];
static int norder = 0;
static void make_order(void){
    norder = 0;
    for(int i = 0; i < nrows; i++) order[norder++] = i;
    for(int i = 1; i < norder; i++){
        int v = order[i], j = i - 1;
        while(j >= 0){
            const row_t *a = &rows[order[j]], *b = &rows[v];
            int gt;
            if(sort_col == 1) gt = name_cmp(a->name, b->name) > 0;
            else gt = key_of(a, sort_col) > key_of(b, sort_col);
            if(sort_desc) gt = sort_col == 1 ? name_cmp(a->name, b->name) < 0 : key_of(a, sort_col) < key_of(b, sort_col);
            if(!gt) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    for(int g = 0; g < nghosts; g++) order[norder++] = ROWS_MAX + g;
}
static const row_t *row_at(int k){ return order[k] >= ROWS_MAX ? &ghosts[order[k] - ROWS_MAX] : &rows[order[k]]; }
static int sel_index(void){
    for(int k = 0; k < norder; k++){ const row_t *r = row_at(k); if(order[k] < ROWS_MAX && r->kind == sel_kind && r->id == sel_id) return k; }
    return 0;
}

/* ---- layout ---- */

#define GRAPH_H   132
#define TABLE_ROW 40
static int narrow_layout(rect_t b){ return b.w < 560; }
static rect_t graph_rect(rect_t b, int which){
    u32 w = (b.w - 16) / 2;
    return (rect_t){ b.x + (u32)which * (w + 16), b.y + 28, w, GRAPH_H };
}
static u32 toolbar_y(rect_t b){ return b.y + 28 + GRAPH_H + 16; }
static rect_t new_btn(rect_t b){
    if(narrow_layout(b)) return (rect_t){ b.x + b.w - 232, toolbar_y(b), 112, 40 };
    return (rect_t){ b.x + b.w - 380, toolbar_y(b), 180, 40 };
}
static rect_t end_btn(rect_t b){
    if(narrow_layout(b)) return (rect_t){ b.x + b.w - 114, toolbar_y(b), 114, 40 };
    return (rect_t){ b.x + b.w - 188, toolbar_y(b), 188, 40 };
}
static u32 head_y(rect_t b){ return toolbar_y(b) + 56; }
static int table_rows(rect_t b){
    int h = (int)(b.y + b.h) - (int)head_y(b) - 36 - 40;
    return h > 0 ? h / TABLE_ROW : 0;
}
/* column x positions. Wide: pid, name, rss, group rss, cpu, group cpu, end.
 * Narrow: pid, name, rss, cpu - the group columns are dropped so no text can
 * run into its neighbour. */
static void cols(rect_t b, u32 *x){
    u32 right = b.x + b.w;
    if(!narrow_layout(b)){
        /* pid 70, name (at least 140), rss 120, group rss 140, cpu 100, group cpu 110 */
        x[6] = right;
        x[5] = right - 110;
        x[4] = x[5] - 100;
        x[3] = x[4] - 140;
        x[2] = x[3] - 120;
        x[1] = b.x + 70;
        x[0] = b.x;
    } else {
        /* pid 58 (room for the sort arrow) | name | rss 76 | cpu 60 */
        x[0] = b.x;
        x[1] = b.x + 58;
        x[2] = right - 136;
        x[3] = right - 60;
        x[4] = right;
        x[5] = right;
        x[6] = right;
    }
}

/* ---- drawing ---- */

static void fmt_bytes(char *out, u64 v){
    char n[16];
    out[0] = 0;
    if(v >= 1024 * 1024){
        u64 t = v * 10 / (1024 * 1024);
        ui_num((u32)(t / 10), n); ui_cat(out, n); ui_cat(out, ".");
        ui_num((u32)(t % 10), n); ui_cat(out, n); ui_cat(out, " MB");
    } else {
        ui_num((u32)((v + 1023) / 1024), n); ui_cat(out, n); ui_cat(out, " KB");
    }
}
static void fmt_pct(char *out, u32 tenths){
    char n[12];
    out[0] = 0;
    ui_num(tenths / 10, n); ui_cat(out, n); ui_cat(out, ".");
    ui_num(tenths % 10, n); ui_cat(out, n); ui_cat(out, "%");
}
static void fmt_mb_num(char *out, u64 v){      /* "29.6" for a "29.6 MB" value */
    fmt_bytes(out, v);
    for(char *q = out; *q; q++) if(*q == ' '){ *q = 0; break; }
}

/* A filled history graph with dotted grid lines, newest on the right. */
static void graph(rect_t g, const u16 *h, int n, u32 fill, u32 line){
    ui_fill(g, G_BG);
    ui_fill((rect_t){ g.x, g.y, g.w, 1 }, 0x2A3036);
    ui_fill((rect_t){ g.x, g.y + g.h - 1, g.w, 1 }, 0x2A3036);
    ui_fill((rect_t){ g.x, g.y, 1, g.h }, 0x2A3036);
    ui_fill((rect_t){ g.x + g.w - 1, g.y, 1, g.h }, 0x2A3036);
    u32 ih = g.h - 2;
    for(int q = 1; q < 4; q++){
        u32 y = g.y + 1 + ih * (u32)q / 4;
        for(u32 x = g.x + 2; x + 2 < g.x + g.w; x += 6) ui_fill((rect_t){ x, y, 2, 1 }, G_GRID);
    }
    if(n < 1) return;
    u32 iw = g.w - 2;
    int prev_y = -1;
    for(u32 px = 0; px < iw; px++){
        /* the sample under this column (the graph fills from the right as samples arrive) */
        int span = HIST - 1;
        int pos = (int)((u64)px * (u64)span / (iw - 1));       /* 0..HIST-1 */
        int idx = pos - (HIST - n);
        if(idx < 0) continue;
        u32 v = h[idx];
        if(idx + 1 < n){                                        /* smooth between samples */
            u32 frac = (u32)(((u64)px * span * 256 / (iw - 1)) & 255);
            v = (h[idx] * (256 - frac) + h[idx + 1] * frac) / 256;
        }
        u32 bar = ih * (v > 1000 ? 1000 : v) / 1000;
        u32 x = g.x + 1 + px;
        u32 ytop = g.y + 1 + ih - bar;
        if(bar) ui_fill((rect_t){ x, ytop, 1, bar }, fill);
        int y = (int)ytop;
        int y0 = prev_y < 0 ? y : prev_y;
        int lo = y0 < y ? y0 : y, hi = y0 < y ? y : y0;
        ui_fill((rect_t){ x, (u32)lo, 1, (u32)(hi - lo + 2) }, line);
        prev_y = y;
    }
}

static void dot(u32 x, u32 y, u32 d, u32 c){ ui_rrect(x, y, d, d, d / 2, c); }

static void t_tick(void){
    u64 now = timer_ticks();
    if(!sampled || now - last_sample >= SAMPLE_TICKS){ sample(now); ui_redraw(); }
}

static void t_open(void){
    sampled = 0; nhist = 0; nghosts = 0; nrows = 0; picking = 0;
    sel_kind = K_KERNEL; sel_id = 0; top = 0;
    sample(timer_ticks());
}

static void t_draw(rect_t b, int focused){
    (void)focused;
    last_body = b;
    u64 now = timer_ticks();
    make_order();

    /* graphs */
    int nar = narrow_layout(b);
    char t[64], v[24];
    rect_t gc = graph_rect(b, 0), gm = graph_rect(b, 1);
    ui_copy(t, "CPU  ", sizeof t); fmt_pct(v, cpu_now); ui_cat(t, v);
    ui_text(gc.x, b.y, t, C_TEXT, 1);
    if(nar){
        /* the full "29.6 MB / 256.0 MB" does not fit half of a narrow window */
        ui_copy(t, "MEM ", sizeof t);
        fmt_mb_num(v, mem_used); ui_cat(t, v); ui_cat(t, "/");
        fmt_mb_num(v, mem_total); ui_cat(t, v); ui_cat(t, " MB");
    } else {
        ui_copy(t, "Memory  ", sizeof t); fmt_bytes(v, mem_used); ui_cat(t, v); ui_cat(t, " / ");
        fmt_bytes(v, mem_total); ui_cat(t, v);
    }
    ui_text(gm.x, b.y, t, C_TEXT, 1);
    graph(gc, cpu_hist, nhist, G_FILL, G_LINE);
    graph(gm, mem_hist, nhist, M_FILL, M_LINE);
    ui_text(gc.x + gc.w - ui_text_w("60 s", 1) - 6, gc.y + gc.h + 2, "60 s", C_DIM, 1);

    /* toolbar */
    char cnt[40] = "", n[12];
    ui_num((u32)nrows, n); ui_cat(cnt, n); ui_cat(cnt, nrows == 1 ? " task" : " tasks");
    u32 bs = nar ? 1 : UI_SCALE;
    u32 bt = toolbar_y(b) + (nar ? 12 : 4);
    ui_text(b.x, bt, cnt, C_MUTED, bs);
    rect_t nb = new_btn(b), eb = end_btn(b);
    ui_rrect(nb.x, nb.y, nb.w, nb.h, 10, picking ? ui_accent() : C_CARD_HI);
    ui_text(nb.x + (nb.w - ui_text_w("New task", bs)) / 2, bt, "New task", picking ? C_BAR : C_TEXT, bs);
    ui_rrect(eb.x, eb.y, eb.w, eb.h, 10, 0x5A2A28);
    ui_text(eb.x + (eb.w - ui_text_w("End task", bs)) / 2, bt, "End task", 0xFFB4AB, bs);

    /* table header, with the sort arrow */
    u32 cx[7];
    cols(b, cx);
    u32 hy = head_y(b);
    static const char *hd6[6] = { "PID", "NAME", "RSS", "GROUP RSS", "CPU", "GROUP CPU" };
    static const char *hd4[4] = { "PID", "NAME", "RSS", "CPU" };
    static const int map4[4] = { 0, 1, 2, 4 };
    int ncol = nar ? 4 : 6;
    const char **hd = nar ? hd4 : hd6;
    ui_fill((rect_t){ b.x, hy, b.w, 32 }, C_CARD);
    for(int c = 0; c < ncol; c++){
        int sc = nar ? map4[c] : c;
        u32 tx = c < 2 ? cx[c] + 12 : cx[c + 1] - 14 - ui_text_w(hd[c], 1) - (sort_col == sc ? 16 : 0);
        ui_text(tx, hy + 8, hd[c], sort_col == sc ? C_TEXT : C_MUTED, 1);
        if(sort_col == sc){
            u32 ax = tx + ui_text_w(hd[c], 1) + 6, ay = hy + 11;
            for(u32 k = 0; k < 5; k++){
                u32 w = sort_desc ? 9 - 2 * k : 1 + 2 * k;
                ui_fill((rect_t){ ax + (9 - w) / 2, ay + k * 2, w, 2 }, C_TEXT);
            }
        }
        if(c > 0) ui_fill((rect_t){ cx[c], hy + 6, 1, 20 }, C_SEP);
    }

    /* rows */
    int vis = table_rows(b);
    int si = sel_index();
    if(si < top) top = si;
    if(si >= top + vis) top = si - vis + 1;
    if(top > norder - vis) top = norder - vis;
    if(top < 0) top = 0;
    for(int i = 0; i < vis && top + i < norder; i++){
        int k = top + i;
        const row_t *r = row_at(k);
        int ghost = order[k] >= ROWS_MAX;
        u32 y = hy + 36 + (u32)i * TABLE_ROW;
        rect_t rr = { b.x, y, b.w, TABLE_ROW - 4 };
        u32 bg = k == si && !ghost ? C_CARD_HI : C_WIN;
        /* the state colour: ending, starting, changing */
        u32 mark = 0;
        if(ghost) mark = COL_END;
        else if(r->born && now - r->born < MARK_TICKS) mark = COL_START;
        else if(r->changed && now - r->changed < SAMPLE_TICKS + 10) mark = COL_CHANGE;
        if(mark) bg = ui_lerp(bg, mark, 60);
        ui_rrect(rr.x, rr.y, rr.w, rr.h, 8, bg);
        if(k == si && !ghost) ui_fill((rect_t){ rr.x, rr.y + 6, 4, rr.h - 12 }, ui_accent());
        u32 ts = nar ? 1 : UI_SCALE;   /* scale 1 keeps the numbers inside narrow columns */
        u32 pad = nar ? 8 : 14;
        u32 ty = y + (TABLE_ROW - 4 - (ts == 1 ? 16 : UI_CH)) / 2;
        u32 tc = ghost ? COL_END : C_TEXT;
        /* pid: apps run inside the kernel, so they show its pid */
        char pid[12] = "";
        ui_num((u32)(r->kind == K_PROC ? r->id : 0), pid);
        ui_text(cx[0] + 12, ty, pid, C_MUTED, ts);
        /* name, indented under its parent, with a dot for its state */
        u32 nx = cx[1] + 12 + (r->parent >= 0 ? 24 : 0);
        u32 dc = ghost ? COL_END : ui_streq(r->state, "zombie") ? COL_END :
                 ui_streq(r->state, "blocked") ? C_DIM : ui_streq(r->state, "app") ? ui_accent() : COL_START;
        dot(nx, ty + (ts == 1 ? 5 : 11), ts == 1 ? 6 : 10, dc);
        char name[20];
        u32 room = (cx[2] - nx - (nar ? 8 : 30)) / (ts == 1 ? 8 : UI_CW);
        ui_copy(name, r->name, room + 1 < sizeof name ? room + 1 : sizeof name);
        ui_text(nx + 18, ty, name, tc, ts);
        /* numbers, right-aligned */
        char s[24];
        if(nar){
            fmt_bytes(s, r->rss);   ui_text(cx[3] - pad - ui_text_w(s, ts), ty, s, tc, ts);
            fmt_pct(s, r->cpu);     ui_text(cx[4] - pad - ui_text_w(s, ts), ty, s, tc, ts);
        } else {
            fmt_bytes(s, r->rss);   ui_text(cx[3] - 14 - ui_text_w(s, ts), ty, s, tc, ts);
            fmt_bytes(s, r->grss);  ui_text(cx[4] - 14 - ui_text_w(s, ts), ty, s, ghost ? tc : C_MUTED, ts);
            fmt_pct(s, r->cpu);     ui_text(cx[5] - 14 - ui_text_w(s, ts), ty, s, tc, ts);
            fmt_pct(s, r->gcpu);    ui_text(cx[6] - 14 - ui_text_w(s, ts), ty, s, ghost ? tc : C_MUTED, ts);
        }
    }

    /* legend */
    u32 ly = b.y + b.h - 26, lx = b.x;
    static const char *ln6[3] = { "Starting task", "Changing task", "Terminating task" };
    static const char *ln4[3] = { "Starting", "Changing", "Ending" };
    static const u32 lc[3] = { COL_START, COL_CHANGE, COL_END };
    const char **ln = nar ? ln4 : ln6;
    for(int i = 0; i < 3; i++){
        dot(lx, ly + 2, 14, lc[i]);
        ui_text(lx + 22, ly, ln[i], C_MUTED, 1);
        lx += 22 + ui_text_w(ln[i], 1) + (nar ? 16 : 28);
    }

    /* how the screen itself is doing: redraws in the last second and what one cost */
    if(!nar){
        ui_perf_t pf;
        ui_perf(&pf);
        char line[80] = "Screen: ", n[12];
        ui_num(pf.redraws, n); ui_cat(line, n); ui_cat(line, " redraws/s, ");
        ui_num(pf.avg_us / 1000, n); ui_cat(line, n); ui_cat(line, ".");
        ui_num(pf.avg_us % 1000 / 100, n); ui_cat(line, n); ui_cat(line, " ms each, input ");
        ui_num(pf.loops, n); ui_cat(line, n); ui_cat(line, " Hz");
        if(lx + ui_text_w(line, 1) < b.x + b.w) ui_text_right(b, ly, line, C_DIM, 1);
    }

    /* "New task": the programs on the disk */
    if(picking){
        u32 pw = 340, ph = 56 + (u32)(nprogs ? nprogs : 1) * 40;
        u32 px = nb.x + nb.w - pw, py = nb.y + nb.h + 6;
        ui_rrect(px + 3, py + 6, pw, ph, 12, C_SHADOW);
        ui_rrect(px, py, pw, ph, 12, C_BAR_EDGE);
        ui_rrect(px + 1, py + 1, pw - 2, ph - 2, 11, C_MENU);
        ui_text(px + 16, py + 12, "Run a program from the disk", C_MUTED, 1);
        if(!nprogs) ui_text(px + 16, py + 44, "No .ELF files in /", C_DIM, UI_SCALE);
        for(int i = 0; i < nprogs; i++)
            ui_text(px + 20, py + 40 + (u32)i * 40, progs[i], C_TEXT, UI_SCALE);
    }
}

/* ---- actions ---- */

static void list_cb(const char *name, int is_dir, u32 size){
    (void)size;
    if(is_dir || nprogs >= 16) return;
    u32 n = ui_len(name);
    if(n < 5 || !(name[n - 4] == '.' && (name[n - 3] == 'E' || name[n - 3] == 'e'))) return;
    ui_copy(progs[nprogs++], name, sizeof progs[0]);
}

static void start_prog(int i){
    char path[24] = "";                 /* root-level files have no leading slash */
    ui_cat(path, progs[i]);
    int pid = elf_spawn(path);
    char m[64] = "";
    if(pid >= 0){
        char n[12];
        ui_num((u32)pid, n);
        ui_cat(m, progs[i]); ui_cat(m, " is running as pid "); ui_cat(m, n);
        ui_notify(ui_app_index(&app_tasks), "Task started", m);
        sel_kind = K_PROC; sel_id = pid;
    } else {
        ui_cat(m, "Could not start "); ui_cat(m, progs[i]);
        ui_notify(-1, "Task not started", m);
    }
    sample(timer_ticks());
}

static void end_selected(void){
    char m[64] = "", n[12];
    if(sel_kind == K_KERNEL){ ui_notify(-1, "Not ended", "The kernel runs the whole system"); return; }
    if(sel_kind == K_APP){
        ui_copy(m, ui_apps[sel_id]->name, sizeof m);
        if(ui_apps[sel_id] == &app_tasks){ ui_notify(-1, "Not ended", "Close Tasks with its window button"); return; }
        ui_app_close(sel_id);
        ui_cat(m, " was closed");
        ui_notify(-1, "Task ended", m);
    } else {
        ui_num((u32)sel_id, n);
        if(proc_kill(sel_id) == 0){ ui_cat(m, "Killed pid "); ui_cat(m, n); ui_notify(-1, "Task ended", m); }
        else { ui_cat(m, "Could not kill pid "); ui_cat(m, n); ui_notify(-1, "Task not ended", m); }
    }
    sample(timer_ticks());
    sel_kind = K_KERNEL; sel_id = 0;
}

static void move_sel(int d){
    make_order();
    int k = sel_index() + d;
    int live = norder - nghosts;
    if(k < 0) k = 0;
    if(k >= live) k = live - 1;
    if(k < 0) return;
    const row_t *r = row_at(k);
    sel_kind = r->kind; sel_id = r->id;
}

static int t_key(int c){
    if(picking && c == 27){ picking = 0; return 1; }
    if(c == KEY_UP)   { move_sel(-1); return 1; }
    if(c == KEY_DOWN) { move_sel(1); return 1; }
    if(c == KEY_PGUP) { move_sel(-table_rows(last_body)); return 1; }
    if(c == KEY_PGDN) { move_sel(table_rows(last_body)); return 1; }
    if(c == KEY_DEL || c == 'k' || c == 'K'){ end_selected(); return 1; }
    if(c == 'n' || c == 'N'){ picking = !picking; nprogs = 0; if(picking) fat32_list_path("/", list_cb); return 1; }
    return 0;
}

static void t_click(rect_t b, int x, int y){
    rect_t nb = new_btn(b);
    if(picking){
        u32 pw = 340, px = nb.x + nb.w - pw, py = nb.y + nb.h + 6;
        for(int i = 0; i < nprogs; i++){
            rect_t r = { px, py + 40 + (u32)i * 40 - 4, pw, 40 };
            if(ui_contains(r, x, y)){ picking = 0; start_prog(i); return; }
        }
        picking = 0;
        if(ui_contains(nb, x, y)) return;
    }
    if(ui_contains(nb, x, y)){ picking = 1; nprogs = 0; fat32_list_path("/", list_cb); return; }
    if(ui_contains(end_btn(b), x, y)){ end_selected(); return; }
    u32 cx[7];
    cols(b, cx);
    u32 hy = head_y(b);
    if(y >= (int)hy && y < (int)hy + 32){
        int c;
        if(narrow_layout(b)){
            static const int map4[4] = { 0, 1, 2, 4 };
            int slot = x < (int)cx[1] ? 0 : x < (int)cx[2] ? 1 : x < (int)cx[3] ? 2 : 3;
            c = map4[slot];
        } else {
            c = 0;
            for(int i = 0; i < 6; i++) if(x >= (int)cx[i] && x < (int)cx[i + 1 < 6 ? i + 1 : 6]) c = i;
            if(x >= (int)cx[5]) c = 5;
        }
        if(c == sort_col) sort_desc = !sort_desc;
        else { sort_col = c; sort_desc = c >= 2; }       /* numbers: biggest first */
        return;
    }
    make_order();
    int vis = table_rows(b);
    for(int i = 0; i < vis && top + i < norder; i++){
        u32 ry = hy + 36 + (u32)i * TABLE_ROW;
        if(y >= (int)ry && y < (int)(ry + TABLE_ROW) && order[top + i] < ROWS_MAX){
            const row_t *r = row_at(top + i);
            sel_kind = r->kind; sel_id = r->id;
            return;
        }
    }
}

static void t_scroll(rect_t b, int x, int y, int dz){
    (void)b; (void)x; (void)y;
    move_sel(dz);
}

const app_t app_tasks = {
    .name = "Tasks",
    .icon = UI_ICO_TASKS,
    .open = t_open,
    .close = NULL,
    .draw = t_draw,
    .key = t_key,
    .click = t_click,
    .tick = t_tick,
    .scroll = t_scroll,
};
