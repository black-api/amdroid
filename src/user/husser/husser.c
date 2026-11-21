/* husser.c — the Husser renderer: a ring-3 process that owns the page.
 * The browser (in the kernel) owns the network and the window; this process
 * owns parsing, laying out, scripting and painting the page. If it faults,
 * the kernel kills only it — the desktop keeps running.
 *
 * Talks to the browser through the shared region at HUSSER_SHM_VA. */
#include "amdroid/types.h"
#include "amdroid/husser_proto.h"
#include "amdroid/web.h"
#include "amdroid/js.h"
void heap_init(void *base, u64 bytes);
void web_layout_release(void);
#include "string.h"

/* ---- system calls (same numbers as the kernel's syscall table) ---- */
#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_YIELD 3
#define SYS_TICKS 5
#define SYS_SLEEP 15

static i64 sc3(i64 n, i64 a, i64 b, i64 c){
    i64 r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory", "rcx", "r11");
    return r;
}
/* the JS collector scans from the current frame up to stack_top; here that is
 * the top of the user stack the kernel maps (STACK_TOP in the bridge) */
__asm__(".global stack_top\nstack_top = 0x7F000000");
static void sys_exit(i64 code){ sc3(SYS_EXIT, code, 0, 0); for(;;){} }
static u64 now_ticks(void){ return (u64)sc3(SYS_TICKS, 0, 0, 0); }
static void idle_sleep(void){ sc3(SYS_SLEEP, 1, 0, 0); }   /* 10 ms: no CPU while idle */

static volatile husser_shm_t *const shm = (husser_shm_t *)HUSSER_SHM_VA;

/* A frame is painted here, then copied whole into the shared viewport: the browser,
 * which may look at any moment, never sees a page half painted. */
static u32 frame_buf[HUSSER_MAX_W * HUSSER_MAX_H];
static u32 frame_w, frame_h;

#define RES_FAIL 0xFFFFFFFFu
#define PAGE_SRC ((const u8 *)(HUSSER_SHM_VA + HUSSER_PAGE_OFF))
#define RES_SRC  ((const u8 *)(HUSSER_SHM_VA + HUSSER_RES_OFF))

static int have_page = 0;
static int laid_width = -1;
static int page_h = 0;
static u32 scroll = 0;
static void *hover_node = NULL;
static void *focus_node = NULL;
#define SHEETS 48
static char sheet_done[SHEETS];
/* Loading: resources arrive one after another; the page is laid out again at most every
 * LAYOUT_EVERY ticks while they do (and at once when the last one is in), not once per
 * resource. The first layout waits up to CSS_WAIT ticks for the stylesheets, so the page
 * does not appear unstyled and then jump. */
#define LAYOUT_EVERY 30
#define CSS_WAIT     500
static u64 last_layout_cost = 0;          /* ticks the last layout took: wait at least twice that */
static int relayout_pending = 0;
static int awaiting_css = 0;
static u64 load_tick = 0, last_layout_tick = 0;
static u64 now_ticks(void);
static u32 last_cmd_seq = 0;

void painter_target(u32 *buf, u32 w, u32 h);
void painter_clear(u32 c);
void painter_cmds(const web_cmd_t *cmds, int n, int scroll, const void *hover_node);

static int starts(const char *s, const char *prefix){
    while(*prefix){ if(*s != *prefix) return 0; s++; prefix++; }
    return 1;
}
static void str_copy(char *dst, const char *src, u32 cap){
    u32 i = 0;
    if(cap == 0) return;
    for(; i < cap - 1 && src && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ---- page painting ---- */
static void paint_frame(void){
    if(!have_page){ painter_clear(0x10181D); return; }
    u32 canvas = 0xFFFFFF;                         /* a page that sets no background is white, as in any browser */
    web_canvas_bg(&canvas);
    painter_clear(canvas);                         /* the page background fills the whole window */
    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    painter_cmds(cmds, n, (int)scroll, hover_node);
    /* the <video> box, where the browser draws the picture (viewport coordinates) */
    int vx, vy, vw, vh;
    if(web_video_box(&vx, &vy, &vw, &vh)){ shm->video_x = vx; shm->video_y = vy - (int)scroll; shm->video_w = vw; shm->video_h = vh; }
    else shm->video_w = 0;
    shm->video_seq++;
}

/* ---- fetch() and media: the page's requests go to the browser, its answers come back ---- */
static int media_inflight = 0;
static u32 ms_seen = 0;
static u64 ms_tick = 0;
static void media_pump(void){
    /* fetch requests: rewritten whenever the list changes (the browser takes new ids only) */
    web_fetch_req_t rq[HUSSER_FETCH_MAX];
    int changed;
    int n = web_fetch_requests(rq, HUSSER_FETCH_MAX, &changed);
    if(changed){
        for(int k = 0; k < n; k++){
            u8 *ent = (u8 *)(HUSSER_SHM_VA + HUSSER_FETCHQ_OFF + (u64)k * HUSSER_FETCH_ENT);
            u32 id = rq[k].id;
            ent[0] = id & 255; ent[1] = id >> 8 & 255; ent[2] = id >> 16 & 255; ent[3] = id >> 24;
            u32 at = 4;
            str_copy((char *)ent + at, rq[k].url, HUSSER_FETCH_URL);
            at += (u32)strlen((char *)ent + at) + 1;
            str_copy((char *)ent + at, rq[k].method, 12);
            at += (u32)strlen((char *)ent + at) + 1;
            str_copy((char *)ent + at, rq[k].ctype, 128);
            at += (u32)strlen((char *)ent + at) + 1;
            u32 bl = rq[k].blen <= HUSSER_FETCH_ENT - at - 4 ? rq[k].blen : 0;
            ent[at] = bl & 255; ent[at + 1] = bl >> 8 & 255; ent[at + 2] = bl >> 16 & 255; ent[at + 3] = bl >> 24;
            if(bl) memcpy(ent + at + 4, rq[k].body, bl);
        }
        shm->fetch_count = (u32)n;
        shm->fetch_seq++;
    }
    /* document.cookie writes: appended after a length word, for the browser to put in its jar */
    {
        static char cw[2048];
        int n = web_cookie_writes(cw, sizeof cw);
        if(n > 0){
            u32 *len = (u32 *)(HUSSER_SHM_VA + HUSSER_COOKIE_OFF + HUSSER_COOKIE_IN);
            char *dst = (char *)(len + 1);
            if(*len + (u32)n <= HUSSER_COOKIE_BYTES - HUSSER_COOKIE_IN - 4){
                memcpy(dst + *len, cw, (u32)n);
                *len += (u32)n;
                shm->cookie_out_seq++;
            }
        }
    }
    /* media commands, one at a time */
    if(media_inflight && shm->media_ack == shm->media_seq){ web_media_acked(shm->ms_seq); media_inflight = 0; }
    if(!media_inflight){
        u32 cmd, len; i32 a; i64 t0, t1; const u8 *data;
        if(web_media_next(&cmd, &a, &t0, &t1, &data, &len)){
            if(data && len) memcpy((void *)(HUSSER_SHM_VA + HUSSER_MEDIA_OFF), data, len);
            shm->media_cmd = cmd; shm->media_a = a; shm->media_t0 = t0; shm->media_t1 = t1; shm->media_len = len;
            web_media_pop();
            __asm__ volatile("" ::: "memory");
            shm->media_seq++;
            media_inflight = 1;
        }
    }
    /* the state, ten times a second, to the page's script */
    u64 now = now_ticks();
    if(shm->ms_seq != ms_seen && now >= ms_tick + 10){
        ms_seen = shm->ms_seq;
        ms_tick = now;
        web_mstate_t st;
        st.seq = shm->ms_seq;
        st.time_us = shm->ms_time_us; st.duration_us = shm->ms_duration_us;
        st.playing = shm->ms_playing; st.ended = shm->ms_ended; st.stalled = shm->ms_stalled; st.ready = shm->ms_ready;
        st.video_w = shm->ms_video_w; st.video_h = shm->ms_video_h;
        for(int k = 0; k < 2; k++){
            st.pending[k] = shm->ms_pending[k];
            st.nranges[k] = shm->ms_nranges[k] > 8 ? 8 : shm->ms_nranges[k];
            for(u32 i = 0; i < st.nranges[k] * 2; i++) st.ranges[k][i] = shm->ms_ranges[k][i];
        }
        st.error_seq = shm->ms_error_seq;
        st.error = (const char *)shm->ms_error;
        web_media_state(&st);
    }
}
static void update_stats(void){
    shm->page_h = (u32)page_h;
    const char *t = web_title();
    if(t && strcmp(t, shm->title) != 0){ str_copy((char *)shm->title, t, HUSSER_TITLE_MAX); shm->title_seq++; }
    const char *e = web_js_error();
    if(e && e[0] && strcmp(e, shm->js_error) != 0){ str_copy((char *)shm->js_error, e, HUSSER_MSG_MAX); shm->js_error_seq++; }
    const char *c = web_console();
    if(c && c[0] && strcmp(c, shm->console) != 0){ str_copy((char *)shm->console, c, HUSSER_MSG_MAX); shm->console_seq++; }
}
static void set_size(u32 w, u32 h){
    frame_w = w; frame_h = h;
    painter_target(frame_buf, w, h);
}
static void frame_ready(void){
    update_stats();
    if(shm->frame_w != frame_w || shm->frame_h != frame_h) shm->frame_w = shm->frame_h = 0;   /* rows change length */
    memcpy((void *)(HUSSER_SHM_VA + HUSSER_VRAM_OFF), frame_buf, (u64)frame_w * frame_h * 4);
    shm->frame_w = frame_w; shm->frame_h = frame_h;
    shm->state = H_STATE_READY;
    shm->frame_seq++;
}
/* a DOM change or a pending relayout: re-lay out at the current width, then paint */
static u32 clamp_scroll(u32 s){
    int mx = page_h - (int)shm->vh;
    if(mx < 0) mx = 0;
    return s > (u32)mx ? (u32)mx : s;
}
static void relayout_if_dirty(void){
    if(web_dirty() || laid_width != (int)shm->vw){
        page_h = web_layout((int)shm->vw);
        laid_width = (int)shm->vw;
        web_clear_dirty();
        scroll = clamp_scroll(scroll);
    }
    paint_frame();
}

/* ---- hit testing (only text runs carry clickable nodes) ---- */
static const void *hit_node(i32 x, i32 y, int *out_link){
    *out_link = -1;
    if(!have_page) return NULL;
    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    int px = x, py = y + (int)scroll;
    const void *found = NULL;
    int found_link = -1;
    for(int i = 0; i < n; i++){
        const web_cmd_t *c = &cmds[i];
        if(!c->dom) continue;                      /* text, images and element boxes can be hit */
        if(px >= c->x && px < c->x + c->w && py >= c->y && py < c->y + c->h){ found = c->dom; found_link = c->link; }
    }
    *out_link = found_link;
    return found;
}

/* ---- resources: the browser fetches, we decode ---- */
static int sheets_pending(void){
    int ns = web_stylesheet_count(), n = 0;
    for(int i = 0; i < ns && i < SHEETS; i++){
        if(sheet_done[i]) continue;
        const char *href = web_stylesheet(i);
        if(!href || !href[0] || starts(href, "data:")){ sheet_done[i] = 1; continue; }
        n++;
    }
    return n;
}
/* Anything the page still waits for (stylesheets, scripts, images). */
static int outstanding(void){
    int n = sheets_pending() + web_scripts_pending();
    int ni = web_image_count();
    for(int i = 0; i < ni; i++){
        if(web_image_state(i) != 0 || !web_image_wanted(i)) continue;
        const char *href = web_image(i);
        if(href && href[0] && !starts(href, "data:")) n++;
    }
    return n;
}

static void emit_needs(void){
    if(!have_page) return;
    u32 slot = 0;
    /* stylesheets first: the first layout waits for them */
    int ns = web_stylesheet_count();
    for(int i = 0; i < ns && slot < HUSSER_NEED_MAX && i < SHEETS; i++){
        if(sheet_done[i]) continue;
        const char *href = web_stylesheet(i);
        if(!href || !href[0] || starts(href, "data:")){ sheet_done[i] = 1; continue; }
        shm->need[slot].kind = 1;
        shm->need[slot].idx = (u32)i;
        str_copy(shm->need[slot].url, href, HUSSER_URL_NEED);
        slot++;
    }
    /* then scripts: the page's scripts run once every external one is here */
    int nsc = web_script_count();
    for(int i = 0; i < nsc && slot < HUSSER_NEED_MAX; i++){
        const char *src = web_script_src(i);
        if(!src) continue;
        if(starts(src, "data:")){ web_add_script(i, "", 0); continue; }
        shm->need[slot].kind = 3;
        shm->need[slot].idx = (u32)i;
        str_copy(shm->need[slot].url, src, HUSSER_URL_NEED);
        slot++;
    }
    if(nsc && !web_scripts_pending() && web_run_scripts()) relayout_pending = 1;
    int ni = web_image_count();
    for(int i = 0; i < ni && slot < HUSSER_NEED_MAX; i++){
        if(web_image_state(i) != 0 || !web_image_wanted(i)) continue;   /* only images a layout showed */
        const char *href = web_image(i);
        if(!href || !href[0]) continue;
        if(starts(href, "data:")){ web_add_image_data_url(i, href); relayout_pending = 1; continue; }
        shm->need[slot].kind = 2;
        shm->need[slot].idx = (u32)i;
        str_copy(shm->need[slot].url, href, HUSSER_URL_NEED);
        slot++;
    }
    shm->need_count = slot;
    shm->need_seq++;
}

/* Lays the page out again and shows it. */
static void emit_needs(void);
static void layout_now(void){
    u64 t0 = now_ticks();
    page_h = web_layout((int)shm->vw);
    laid_width = (int)shm->vw;
    web_clear_dirty();
    scroll = clamp_scroll(scroll);
    paint_frame();
    frame_ready();
    relayout_pending = 0;
    last_layout_tick = now_ticks();
    last_layout_cost = last_layout_tick - t0;
    emit_needs();                             /* the layout may have shown images not asked for yet */
}

static void load_page(u32 len, u32 initial_scroll){
    if(len > HUSSER_PAGE_BYTES) len = HUSSER_PAGE_BYTES;
    web_set_url((const char *)shm->page_url);
    web_set_viewport((int)shm->vw, (int)shm->vh);
    web_set_adblock((int)shm->adblock);
    {   /* the page's cookies (the browser wrote them before H_CMD_LOAD) */
        char *ck = (char *)(HUSSER_SHM_VA + HUSSER_COOKIE_OFF);
        ck[HUSSER_COOKIE_IN - 1] = 0;
        web_set_cookies(ck);
        *(u32 *)(HUSSER_SHM_VA + HUSSER_COOKIE_OFF + HUSSER_COOKIE_IN) = 0;   /* no writes yet */
    }
    web_layout_release();                     /* nothing of the old page can be hit or painted */
    if(web_load((const char *)PAGE_SRC, len) != 0){
            str_copy((char *)shm->js_error, "This page could not be parsed.", HUSSER_MSG_MAX);
        shm->js_error_seq++;
        shm->state = H_STATE_FAILED;
        return;
    }
    have_page = 1;
    focus_node = hover_node = NULL;
    shm->input_focus = 0;
    /* the last page's script error and console line are not this page's */
    if(shm->js_error[0]){ shm->js_error[0] = 0; shm->js_error_seq++; }
    if(shm->console[0]){ shm->console[0] = 0; shm->console_seq++; }
    for(int i = 0; i < SHEETS; i++) sheet_done[i] = 0;
    load_tick = now_ticks();
    relayout_pending = 0;
    scroll = initial_scroll;
    awaiting_css = sheets_pending() > 0;
    emit_needs();
    if(!awaiting_css) layout_now();
    else update_stats();                      /* the title shows while the stylesheets come */
}

static void res_done(void){
    if(!have_page) return;
    u32 kind = shm->res_kind, idx = shm->res_idx, len = shm->res_len;
    if(kind == 4){                                      /* a script's fetch(): its promise settles */
        web_fetch_done(idx, shm->res_status, RES_SRC, len == RES_FAIL ? 0 : len, len == RES_FAIL);
        if(web_dirty()) relayout_pending = 1;
        return;
    }
    if(len != RES_FAIL){
        if(kind == 1 && idx < SHEETS){ web_add_css_at((int)idx, (const char *)RES_SRC, len); sheet_done[idx] = 1; }
        else if(kind == 2){ web_add_image((int)idx, (const char *)RES_SRC, len); }
        else if(kind == 3){ web_add_script((int)idx, (const char *)RES_SRC, len); }
    } else {
        if(kind == 1 && idx < SHEETS) sheet_done[idx] = 1;  /* the failed sheet is left out */
        else if(kind == 2) web_add_image((int)idx, "", 0);  /* the failed image shows its alt text */
        else if(kind == 3) web_add_script((int)idx, "", 0); /* the failed script is skipped */
    }
    relayout_pending = 1;
    emit_needs();                                       /* the browser fetches the next ones meanwhile */
    if(awaiting_css){
        if(sheets_pending() == 0){ awaiting_css = 0; layout_now(); }
        return;
    }
    if(outstanding() == 0 || now_ticks() >= last_layout_tick + LAYOUT_EVERY + 2 * last_layout_cost) layout_now();
}

static void do_click(i32 x, i32 y){
    int link = -1;
    const void *node = hit_node(x, y, &link);
    if(node){
        web_click(node);
        if(web_is_input(node)){ focus_node = (void *)node; shm->input_focus = 1; web_activate(node); relayout_if_dirty(); frame_ready(); return; }
        focus_node = NULL;
        shm->input_focus = 0;
        if(web_take_prevented()){ relayout_if_dirty(); frame_ready(); return; }
        const char *href = link >= 0 ? web_link(link) : NULL;
        if(href && href[0] && !starts(href, "javascript:") && !starts(href, "mailto:") && !(href[0] == '#' && href[1] == 0)){
            str_copy((char *)shm->nav_url, href, HUSSER_URL_MAX);
            shm->nav_seq++;
        } else {
            /* not a link: buttons open their menus, a search button shows its box */
            const void *box = web_activate(node);
            if(box && web_is_input(box)){ focus_node = (void *)box; shm->input_focus = 1; }
        }
        relayout_if_dirty();
        frame_ready();
        return;
    }
    focus_node = NULL;
    shm->input_focus = 0;
    web_activate(NULL);                           /* a click on nothing closes an open menu */
    relayout_if_dirty();
    frame_ready();
}

static void do_move(i32 x, i32 y){
    int link;
    const void *node = hit_node(x, y, &link);
    if(node == hover_node) return;
    if(hover_node) web_dispatch(hover_node, "mouseout", NULL);
    hover_node = (void *)node;
    if(hover_node) web_dispatch(hover_node, "mouseover", NULL);
    if(web_dirty()){ relayout_if_dirty(); } else { paint_frame(); }   /* repaint for the underline */
    frame_ready();
}

static void do_key(i32 code){
    if(focus_node){
        if(web_key_input(focus_node, code)){
            relayout_if_dirty();
            frame_ready();
        }
        return;
    }
    /* nothing to focus: keep focus_node in the engine for inputs clicked earlier */
}

/* the kernel jumps in without a call, so rsp is 16-aligned at entry and every
 * C prologue would be off by 8, crashing on movdqa. Align and call in. */
void husser_main(void);
void _start(void) __attribute__((naked, noreturn));
void _start(void){
    __asm__ volatile(
        "and $-16, %rsp\n\t"
        "call husser_main\n\t"
        "cli\n\t"
        "hlt\n\t");
}

void husser_main(void){
    if(shm->magic != HUSSER_MAGIC || shm->ver != HUSSER_VER) sys_exit(3);
    heap_init((void *)HUSSER_HEAP_VA, shm->heap_bytes);
    /* the script heap: most of the memory in ultra mode, so the collector rarely runs */
    js_set_heap_size(shm->ultra ? (200u << 20) : (shm->heap_bytes >= (208u << 20) ? (104u << 20) : (shm->heap_bytes / 2)));
    set_size(shm->vw, shm->vh);
    shm->state = H_STATE_BOOT;

    for(;;){
        if(shm->cmd_seq != last_cmd_seq){
            u32 cmd = shm->cmd;
            i32 i = shm->cmd_i, j = shm->cmd_j;
            last_cmd_seq = shm->cmd_seq;
            if(cmd == H_CMD_SHUTDOWN){ shm->ack_seq = last_cmd_seq; sys_exit(0); }
            switch(cmd){
            case H_CMD_LOAD:    load_page((u32)i, (u32)j); break;
            case H_CMD_PAINT:   scroll = clamp_scroll((u32)i); paint_frame(); frame_ready(); break;
            case H_CMD_CLICK:   do_click(i, j); break;
            case H_CMD_MOVE:    do_move(i, j); break;
            case H_CMD_KEY:     do_key(i); break;
            case H_CMD_RESIZE:
                if((u32)i <= HUSSER_MAX_W && (u32)j <= HUSSER_MAX_H && (u32)i > 0 && (u32)j > 0){
                    shm->vw = (u32)i; shm->vh = (u32)j;
                    set_size(shm->vw, shm->vh);
                    scroll = clamp_scroll(scroll);
                    relayout_if_dirty();            /* lays out again only when the width changed */
                    frame_ready();
                }
                break;
            case H_CMD_RES_DONE: res_done(); break;
            case H_CMD_LINK_URL: {
                const char *href = i >= 0 ? web_link((int)i) : NULL;
                str_copy((char *)shm->link_url, href ? href : "", HUSSER_URL_MAX);
                shm->link_url_seq++;
                break; }
            default: break;
            }
            shm->ack_seq = last_cmd_seq;      /* the sender waits for this */
        }
        if(have_page){
            /* a script navigated (location.href = ...): the browser loads it */
            const char *to = web_take_navigation();
            if(to && to[0]){ str_copy((char *)shm->nav_url, to, HUSSER_URL_MAX); shm->nav_seq++; }
        }
        if(have_page){
            /* the stylesheets are late: show the page without them */
            u64 t = now_ticks();
            if(awaiting_css && t >= load_tick + CSS_WAIT){ awaiting_css = 0; layout_now(); }
            /* resources that came in since the last layout */
            if(relayout_pending && !awaiting_css && t >= last_layout_tick + LAYOUT_EVERY + 2 * last_layout_cost) layout_now();
        }
        if(have_page) media_pump();
        if(have_page && !awaiting_css){
            /* scripts changed the page from a timer: lay it out again at most 4 times a second */
            static u64 last_timer_layout = 0;
            static int pending = 0;
            if(web_tick() || web_dirty()) pending = 1;
            u64 now = (u64)sc3(SYS_TICKS, 0, 0, 0);
            if(pending && now >= last_timer_layout + 25){
                pending = 0;
                last_timer_layout = now;
                relayout_if_dirty();
                frame_ready();
            }
        }
        idle_sleep();
    }
}
