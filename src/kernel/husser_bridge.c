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
static u32 husser_vw = 0, husser_vh = 0;

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

static void send_cmd(u32 cmd, i32 i, i32 j){
    const volatile husser_shm_t *h = husser_header();
    if(!h) return;
    volatile husser_shm_t *m = (volatile husser_shm_t *)h;
    m->cmd = cmd; m->cmd_i = i; m->cmd_j = j; m->cmd_seq++;
    u32 want = m->cmd_seq;
    /* wait for husser to finish this command, so commands are never skipped
     * (the browser can queue LOAD and RESIZE before husser's first iteration) */
    for(int t = 0; t < 200; t++){
        if(m->ack_seq == want) return;
        if(!husser_alive()) return;
        proc_sleep_ticks(1);
    }
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
    m->vw = vw; m->vh = vh; m->pitch = vw * 4;
    m->state = H_STATE_BOOT;
    husser_vw = vw; husser_vh = vh;

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

void husser_close(void){
    if(husser_pid < 0) return;
    send_cmd(H_CMD_SHUTDOWN, 0, 0);
    husser_pid = -1;
}

int husser_alive(void){
    if(husser_pid < 0) return 0;
    proc_info_t pi[MAX_PROCS];
    u64 idle = 0;
    int n = proc_info(pi, MAX_PROCS, &idle);
    for(int i = 0; i < n; i++) if(pi[i].pid == husser_pid) return 1;
    husser_pid = -1;                     /* it is gone: stop asking */
    return 0;
}

void husser_set_url(const char *url){
    if(!shm_ready) return;
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    u64 i = 0;
    for(; i < HUSSER_URL_MAX - 1 && url && url[i]; i++) m->page_url[i] = url[i];
    m->page_url[i] = 0;
}

int husser_load(const void *html, u32 len, u32 scroll){
    if(husser_pid < 0 || !shm_ready) return -1;
    if(len > HUSSER_PAGE_BYTES) len = HUSSER_PAGE_BYTES;
    shm_write(HUSSER_PAGE_OFF, html, len);
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->page_len = len;
    m->scroll = scroll;
    send_cmd(H_CMD_LOAD, (i32)len, (i32)scroll);
    return 0;
}

void husser_res(u32 kind, u32 idx, const void *bytes, u32 len){
    if(husser_pid < 0 || !shm_ready) return;
    if(bytes && len){
        if(len > HUSSER_RES_BYTES) len = HUSSER_RES_BYTES;
        shm_write(HUSSER_RES_OFF, bytes, len);
    }
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->res_kind = kind;
    m->res_idx = idx;
    m->res_len = bytes ? len : 0xFFFFFFFFu;
    m->res_done_seq++;
    send_cmd(H_CMD_RES_DONE, 0, 0);
}

void husser_link_url(u32 idx){ if(husser_pid >= 0) send_cmd(H_CMD_LINK_URL, (i32)idx, 0); }

void husser_paint(u32 scroll){
    if(husser_pid < 0) return;
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->scroll = scroll;
    send_cmd(H_CMD_PAINT, (i32)scroll, 0);
}
void husser_key(u32 code){ if(husser_pid >= 0) send_cmd(H_CMD_KEY, (i32)code, 0); }
void husser_click(i32 x, i32 y){ if(husser_pid >= 0) send_cmd(H_CMD_CLICK, x, y); }
void husser_move(i32 x, i32 y){ if(husser_pid >= 0) send_cmd(H_CMD_MOVE, x, y); }
void husser_resize(u32 vw, u32 vh){
    if(husser_pid < 0) return;
    volatile husser_shm_t *m = (volatile husser_shm_t *)(unsigned long)shm_pages[0];
    m->vw = vw; m->vh = vh; m->pitch = vw * 4;
    husser_vw = vw; husser_vh = vh;
    send_cmd(H_CMD_RESIZE, (i32)vw, (i32)vh);
}

void husser_blit(u32 x, u32 y, u32 w, u32 h){
    if(!shm_ready) return;
    if(w > husser_vw) w = husser_vw;
    if(h > husser_vh) h = husser_vh;
    static u32 last_x = 0xFFFFFFFF, last_y = 0xFFFFFFFF, last_w = 0xFFFFFFFF, last_h = 0xFFFFFFFF;
    if(x != last_x || y != last_y || w != last_w || h != last_h){
        console_printf("[blit] x=%u y=%u w=%u h=%u vw=%u vh=%u\n", x, y, w, h, husser_vw, husser_vh);
        /* sample the viewport: what did husser actually paint? */
        {
            u64 o0 = 0;
            u64 om = (u64)(h / 2) * husser_vw * 4 + (u64)(w / 2) * 4;
            u64 o100 = (u64)100 * husser_vw * 4 + (u64)100 * 4;
            console_printf("[blit] px(0,0)=%06x px(w/2,h/2)=%06x px(100,100)=%06x\n",
                *(u32 *)(unsigned long)(shm_pages[HUSSER_VRAM_OFF / 4096 + o0 / 4096] + o0 % 4096),
                *(u32 *)(unsigned long)(shm_pages[HUSSER_VRAM_OFF / 4096 + om / 4096] + om % 4096),
                *(u32 *)(unsigned long)(shm_pages[HUSSER_VRAM_OFF / 4096 + o100 / 4096] + o100 % 4096));
        }
        last_x = x; last_y = y; last_w = w; last_h = h;
    }
    /* the viewport starts at HUSSER_VRAM_OFF; pitch comes from the viewport width */
    fb_copy_region_pages(x, y, w, h, shm_pages + (HUSSER_VRAM_OFF / 4096), husser_vw);
}

u32 husser_vram_nonzero(void){
    if(!shm_ready) return 0;
    u32 count = 0;
    u64 bytes = (u64)husser_vh * husser_vw * 4;
    for(u64 off = HUSSER_VRAM_OFF; off < HUSSER_VRAM_OFF + bytes; off += 4){
        u64 page = off / 4096;
        u32 inner = (u32)(off % 4096);
        if(*(u32*)(unsigned long)(shm_pages[page] + inner)) count++;
    }
    return count;
}
