/* husser_bridge.c — kernel side of the Husser renderer. Owns the shared
 * region (header + viewport + page bytes + one subresource slot) and the
 * lifecycle of the renderer process. The browser app calls these from its
 * own code; nothing here touches the network or the window. */
#include "amdroid/husser.h"
#include "amdroid/elf.h"
#include "amdroid/pmm.h"
#include "amdroid/vm.h"
#include "amdroid/sched.h"
#include "amdroid/fb.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include <string.h>

#define HUSSER_SHM_PAGES (HUSSER_SHM_BYTES / 4096)
#define STACK_TOP        0x7F000000ULL      /* user stack lives far above the image */
#define STACK_PAGES      16                 /* 64 KiB */

extern u8 husser_blob_start[], husser_blob_end[];
extern char husser_bss_end[];               /* from husser_defs.inc */
extern char husser_entry[];                 /* absolute address of _start in the image */

static u64 shm_pages[HUSSER_SHM_PAGES];
static int shm_ready = 0;
static int husser_pid = -1;

static void shm_free_all(void){
    for(u64 i = 0; i < HUSSER_SHM_PAGES; i++) if(shm_pages[i]) pmm_free_page((void*)(unsigned long)shm_pages[i]);
    memset(shm_pages, 0, sizeof shm_pages);
    shm_ready = 0;
}

static int shm_alloc(void){
    for(u64 i = 0; i < HUSSER_SHM_PAGES; i++){
        u64 p = (u64)(unsigned long)pmm_alloc_page();
        if(!p){ shm_free_all(); return -1; }
        shm_pages[i] = p;
    }
    /* the header spans 4 pages (the need[] table is bigger than one page) */
    memset((void*)(unsigned long)shm_pages[0], 0, 4096);
    memset((void*)(unsigned long)shm_pages[1], 0, 4096);
    memset((void*)(unsigned long)shm_pages[2], 0, 4096);
    memset((void*)(unsigned long)shm_pages[3], 0, 4096);
    shm_ready = 1;
    return 0;
}

const volatile husser_shm_t *husser_header(void){
    return shm_ready ? (const volatile husser_shm_t *)(unsigned long)shm_pages[0] : NULL;
}

/* Writes bytes into the shared region at shm offset off. */
static void shm_write(u64 off, const void *src, u64 len){
    const u8 *s = src;
    while(len){
        u64 page = off / 4096;
        u32 inner = (u32)(off % 4096);
        u32 take = 4096 - inner;
        if(take > len) take = (u32)len;
        memcpy((void*)(unsigned long)(shm_pages[page] + inner), s, take);
        s += take; off += take; len -= take;
    }
}

/* ---- commands ----
 * The browser never waits for the renderer: a command goes into a short queue and is handed
 * over when the one before it is done (husser_pump, every tick). A page that takes seconds
 * to lay out keeps Husser busy, not the desktop. */
#define Q_MAX 16
typedef struct { u32 cmd; i32 i, j; } qcmd_t;
static qcmd_t queue[Q_MAX];
static int qn = 0;
static int inflight = 0;            /* a command Husser has not finished */
static u32 inflight_seq = 0;

void husser_pump(void){
    volatile husser_shm_t *m = (volatile husser_shm_t *)husser_header();
    if(!m || !husser_alive()){ qn = 0; inflight = 0; return; }
    if(inflight && m->ack_seq == inflight_seq) inflight = 0;
    if(inflight || !qn) return;
    m->cmd = queue[0].cmd; m->cmd_i = queue[0].i; m->cmd_j = queue[0].j;
    for(int k = 1; k < qn; k++) queue[k - 1] = queue[k];
    qn--;
    inflight_seq = ++m->cmd_seq;
    inflight = 1;
    proc_wake(husser_pid);          /* it sleeps between commands */
}

int husser_busy(void){ return inflight || qn; }

/* Lets Husser run until it has nothing left to do, or for at most `ticks`. */
static void wait_idle(u64 ticks){
    u64 deadline = timer_ticks() + ticks;
    for(husser_pump(); husser_busy() && timer_ticks() < deadline; husser_pump()) proc_yield();
}

static void post(u32 cmd, i32 i, i32 j){
    if(husser_pid < 0) return;
    /* the pointer, the scroll position and the size: only the newest one matters */
    int latest = cmd == H_CMD_MOVE || cmd == H_CMD_PAINT || cmd == H_CMD_RESIZE;
    if(latest && qn && queue[qn - 1].cmd == cmd) qn--;
    if(qn == Q_MAX){
        if(cmd == H_CMD_MOVE) return;
        wait_idle(200);
        if(qn == Q_MAX) return;     /* Husser is stuck; the browser shows that it stopped */
    }
    queue[qn++] = (qcmd_t){ cmd, i, j };
    husser_pump();
}

int husser_open(u32 vw, u32 vh){
    if(husser_pid >= 0) return husser_pid;          /* the browser has one at a time */
    if(vw > HUSSER_MAX_W) vw = HUSSER_MAX_W;
    if(vh > HUSSER_MAX_H) vh = HUSSER_MAX_H;
    if(vw == 0 || vh == 0){ vw = vw ? vw : 800; vh = vh ? vh : 600; }
    if(!shm_ready && shm_alloc() != 0){ console_printf("[husser] out of pages\n"); return -1; }

    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    memset((void*)m, 0, HUSSER_VRAM_OFF);        /* the whole header, zeroed */
    m->magic = HUSSER_MAGIC; m->ver = HUSSER_VER;
    m->vw = vw; m->vh = vh;
    m->state = H_STATE_BOOT;
    qn = 0; inflight = 0;

    u64 cr3 = vm_new_space();
    if(!cr3) return -1;
    /* the blob, page by page, at VM_USER_BASE (its entry is offset 0) */
    u64 va = VM_USER_BASE;
    const u8 *src = husser_blob_start;
    u64 left = (u64)(husser_blob_end - husser_blob_start);
    while(left){
        u64 pa = vm_alloc_user_page(cr3, va);
        if(!pa){ vm_free_space(cr3); return -1; }
        u32 n = left > 4096 ? 4096 : (u32)left;
        memcpy((void*)(unsigned long)pa, src, n);
        src += n; left -= n; va += 4096;
    }
    /* zero pages to the end of the bss (the arena and static pools) */
    u64 bss_end = (u64)(unsigned long)husser_bss_end;
    while(va < bss_end){ if(!vm_alloc_user_page(cr3, va)){ vm_free_space(cr3); return -1; } va += 4096; }
    /* the stack, far above the image */
    for(u32 i = 0; i < STACK_PAGES; i++)
        if(!vm_alloc_user_page(cr3, STACK_TOP - (u64)(i + 1) * 4096)){ vm_free_space(cr3); return -1; }
    /* the shared region at HUSSER_SHM_VA, marked borrowed so the process's
     * address space dies without touching it */
    u64 sva = HUSSER_SHM_VA;
    for(u64 i = 0; i < HUSSER_SHM_PAGES; i++){
        if(vm_map_user(cr3, sva, shm_pages[i], PTE_W | PTE_U | PTE_KEEP) != 0){ vm_free_space(cr3); return -1; }
        sva += 4096;
    }
    int pid = proc_create_user("husser",
                               VM_USER_BASE + ((u64)(unsigned long)husser_entry - 0x40000000ULL),
                               STACK_TOP, cr3, 0);
    if(pid < 0){ vm_free_space(cr3); return -1; }
    proc_detach(pid);
    husser_pid = pid;
    return pid;
}

/* The renderer holds nothing worth saving: it is simply ended, and its memory comes back. */
void husser_close(void){
    if(husser_pid < 0) return;
    proc_kill(husser_pid);
    husser_pid = -1;
    qn = 0; inflight = 0;
}

int husser_alive(void){
    if(husser_pid >= 0 && !proc_alive(husser_pid)) husser_pid = -1;      /* it is gone: stop asking */
    return husser_pid >= 0;
}

void husser_set_url(const char *url){
    if(!shm_ready) return;
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    u64 i = 0;
    for(; i < HUSSER_URL_MAX - 1 && url && url[i]; i++) m->page_url[i] = url[i];
    m->page_url[i] = 0;
}

/* The page and resource areas are read by Husser while it works on them, so they are
 * written only once it has finished what it was doing (normally at once). */
int husser_load(const void *html, u32 len, u32 scroll){
    if(husser_pid < 0 || !shm_ready) return -1;
    if(len > HUSSER_PAGE_BYTES) len = HUSSER_PAGE_BYTES;
    wait_idle(200);
    qn = 0;
    shm_write(HUSSER_PAGE_OFF, html, len);
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->page_len = len;
    post(H_CMD_LOAD, (i32)len, (i32)scroll);
    return 0;
}

void husser_res(u32 kind, u32 idx, const void *bytes, u32 len){
    if(husser_pid < 0 || !shm_ready) return;
    wait_idle(200);
    if(bytes && len){
        if(len > HUSSER_RES_BYTES) len = HUSSER_RES_BYTES;
        shm_write(HUSSER_RES_OFF, bytes, len);
    }
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->res_kind = kind;
    m->res_idx = idx;
    m->res_len = bytes ? len : 0xFFFFFFFFu;
    post(H_CMD_RES_DONE, 0, 0);
}

void husser_link_url(u32 idx){ post(H_CMD_LINK_URL, (i32)idx, 0); }
void husser_paint(u32 scroll){ post(H_CMD_PAINT, (i32)scroll, 0); }
void husser_key(u32 code){ post(H_CMD_KEY, (i32)code, 0); }
void husser_click(i32 x, i32 y){ post(H_CMD_CLICK, x, y); }
void husser_move(i32 x, i32 y){ post(H_CMD_MOVE, x, y); }
void husser_resize(u32 vw, u32 vh){
    if(vw > HUSSER_MAX_W) vw = HUSSER_MAX_W;
    if(vh > HUSSER_MAX_H) vh = HUSSER_MAX_H;
    post(H_CMD_RESIZE, (i32)vw, (i32)vh);
}

/* Copies the frame Husser last finished to (x, y), cut to w x h. Nothing is drawn while
 * the frame is being replaced by one of another size. */
void husser_blit(u32 x, u32 y, u32 w, u32 h){
    const volatile husser_shm_t *m = husser_header();
    if(!m) return;
    u32 fw = m->frame_w, fh = m->frame_h;
    if(!fw || !fh || fw > HUSSER_MAX_W || fh > HUSSER_MAX_H) return;
    fb_copy_region_pages(x, y, w < fw ? w : fw, h < fh ? h : fh, shm_pages + (HUSSER_VRAM_OFF / 4096), fw);
}
