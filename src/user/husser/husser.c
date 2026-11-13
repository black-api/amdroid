/* husser.c — the Husser renderer: a ring-3 process that owns the page.
 * The browser (in the kernel) owns the network and the window; this process
 * owns parsing, laying out, scripting and painting the page. If it faults,
 * the kernel kills only it — the desktop keeps running.
 *
 * Talks to the browser through the shared region at HUSSER_SHM_VA. */
#include "amdroid/types.h"
#include "amdroid/husser_proto.h"
#include "amdroid/web.h"
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
static char sheet_done[16];
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
    painter_clear(0x1C2328);                       /* C_WIN */
    int n = 0;
    const web_cmd_t *cmds = web_cmds(&n);
    painter_cmds(cmds, n, (int)scroll, hover_node);
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
        if(c->kind != WEB_CMD_TEXT) continue;
        if(px >= c->x && px < c->x + c->w && py >= c->y && py < c->y + c->h){ found = c->dom; found_link = c->link; }
    }
    *out_link = found_link;
    return found;
}

/* ---- resources: the browser fetches, we decode ---- */
static void emit_needs(void){
    if(!have_page) return;
    u32 slot = 0;
    int ns = web_stylesheet_count();
    for(int i = 0; i < ns && slot < HUSSER_NEED_MAX && i < 16; i++){
        if(sheet_done[i]) continue;
        const char *href = web_stylesheet(i);
        if(!href || !href[0] || starts(href, "data:")){ sheet_done[i] = 1; continue; }
        shm->need[slot].kind = 1;
        shm->need[slot].idx = (u32)i;
        str_copy(shm->need[slot].url, href, HUSSER_URL_NEED);
        slot++;
    }
    int ni = web_image_count();
    for(int i = 0; i < ni && slot < HUSSER_NEED_MAX; i++){
        if(web_image_state(i) != 0) continue;
        const char *href = web_image(i);
        if(!href || !href[0]) continue;
        if(starts(href, "data:")){ web_add_image_data_url(i, href); continue; }
        shm->need[slot].kind = 2;
        shm->need[slot].idx = (u32)i;
        str_copy(shm->need[slot].url, href, HUSSER_URL_NEED);
        slot++;
    }
    shm->need_count = slot;
    shm->need_seq++;
}

static void load_page(u32 len, u32 initial_scroll){
    if(len > HUSSER_PAGE_BYTES) len = HUSSER_PAGE_BYTES;
    web_set_url((const char *)shm->page_url);
    if(web_load((const char *)PAGE_SRC, len) != 0){
            str_copy((char *)shm->js_error, "This page could not be parsed.", HUSSER_MSG_MAX);
        shm->js_error_seq++;
        shm->state = H_STATE_FAILED;
        return;
    }
    have_page = 1;
    focus_node = hover_node = NULL;
    shm->input_focus = 0;
    for(int i = 0; i < 16; i++) sheet_done[i] = 0;
    page_h = web_layout((int)shm->vw);
    laid_width = (int)shm->vw;
    scroll = clamp_scroll(initial_scroll);
    paint_frame();
    frame_ready();
    emit_needs();
}

static void res_done(void){
    if(!have_page) return;
    u32 kind = shm->res_kind, idx = shm->res_idx, len = shm->res_len;
    if(len != RES_FAIL){
        if(kind == 1 && idx < 16){ web_add_css((const char *)RES_SRC, len); sheet_done[idx] = 1; }
        else if(kind == 2){ web_add_image((int)idx, (const char *)RES_SRC, len); }
        else { if(kind == 1 && idx < 16) sheet_done[idx] = 1; }
    } else {
        if(kind == 1 && idx < 16) sheet_done[idx] = 1;      /* the failed sheet is left out */
        else if(kind == 2) web_add_image((int)idx, "", 0);  /* the failed image shows its alt text */
    }
    page_h = web_layout((int)shm->vw);
    laid_width = (int)shm->vw;
    paint_frame();
    frame_ready();
    emit_needs();
}

static void do_click(i32 x, i32 y){
    int link = -1;
    const void *node = hit_node(x, y, &link);
    if(node){
        web_click(node);
        if(web_is_input(node)){ focus_node = (void *)node; shm->input_focus = 1; relayout_if_dirty(); frame_ready(); return; }
        focus_node = NULL;
        shm->input_focus = 0;
        if(web_take_prevented()){ relayout_if_dirty(); frame_ready(); return; }
        if(link >= 0){
            const char *href = web_link(link);
            if(href && href[0] && !starts(href, "javascript:") && !starts(href, "mailto:")){
                str_copy((char *)shm->nav_url, href, HUSSER_URL_MAX);
                shm->nav_seq++;
            }
        }
        relayout_if_dirty();
        frame_ready();
        return;
    }
    focus_node = NULL;
    shm->input_focus = 0;
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
            if(web_tick()){ relayout_if_dirty(); frame_ready(); }
        }
        idle_sleep();
    }
}
