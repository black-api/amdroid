/* xhci.c — xHCI (USB 3) host controller driver, polled.
 *
 * Enumerates root-hub devices, reads their descriptors, and runs HID boot
 * keyboards: the keyboard's interrupt endpoint is polled and its key reports
 * are turned into characters for the keyboard input queue. */
#include "amdroid/pci.h"
#include "amdroid/vmm.h"
#include "amdroid/pmm.h"
#include "amdroid/keyboard.h"
#include "amdroid/mouse.h"
#include "amdroid/usb.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

/* TRB types */
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define EV_TRANSFER 32
#define EV_CMD_DONE 33
#define EV_PORT 34

#define RING_N 64              /* TRBs per ring (the last one is a link) */

typedef struct { u64 param; u32 status; u32 control; } trb_t;

static volatile u8 *cap;       /* capability registers */
static volatile u8 *op;        /* operational registers */
static volatile u8 *rt;        /* runtime registers */
static volatile u32 *db;       /* doorbells */
static int ctx64;              /* 64-byte contexts (CSZ) */
static int max_ports;

static u64 *dcbaa;
static trb_t *cmd_ring;   static int cmd_i, cmd_cycle = 1;
static trb_t *evt_ring;   static int evt_i, evt_cycle = 1;
static u64 *erst;
static u8 *in_ctx;         /* input context page */

#define MAX_SLOTS 8
typedef struct {
    int used, port, speed;
    u32 route;              /* route string: one nibble per hub level */
    int hub;                /* this device is a hub */
    int tt_slot, tt_port;   /* the hub a slow device sits behind, if any */
    int nports;             /* hub: number of downstream ports */
    int pending;            /* hub: ports with changes waiting for service */
    int hub_dci;            /* hub: interrupt IN endpoint */
    trb_t *hring; int h_i, h_cycle;
    u8 *hreport;
    u8 *dev_ctx;
    trb_t *ep0;  int ep0_i, ep0_cycle;
    int kbd;                /* this slot is a HID keyboard or mouse */
    int hid_kind;           /* 1 keyboard, 2 mouse */
    int kbd_dci;            /* its interrupt IN endpoint's device context index */
    trb_t *kring; int k_i, k_cycle;
    u8 *report;             /* 8-byte boot report buffer */
    u8 prev[8];
    int iface;
    /* mass storage (bulk-only): two bulk endpoints and three transfer buffers */
    int msd;
    int bo_dci, bi_dci;
    trb_t *bo_ring, *bi_ring;
    int bo_i, bo_c, bi_i, bi_c;
    u8 *cbuf, *dbuf, *sbuf;
    u32 tag;
} dev_t_;
static dev_t_ slots[MAX_SLOTS + 1];

/* completion of the last command and of the last control transfer */
static volatile int cmd_done_code, cmd_done_slot;
static volatile int xfer_done_code, xfer_done_slot, xfer_done_ep, xfer_done_resid;

static inline u32 rd32(volatile u8 *b, u32 off){ return *(volatile u32*)(b + off); }
static inline void wr32(volatile u8 *b, u32 off, u32 v){ *(volatile u32*)(b + off) = v; }
static inline void wr64(volatile u8 *b, u32 off, u64 v){ *(volatile u64*)(b + off) = v; }

static void *alloc_page_zeroed(void){
    void *p = pmm_alloc_page();
    if(p) memset(p, 0, 4096);
    return p;
}

/* contexts: 32 or 64 bytes each, index 0 = slot, index n+1 = endpoint DCI n */
static u32 *ctx_at(u8 *base, int index){ return (u32*)(base + (u32)index * (ctx64 ? 64 : 32)); }

/* ---------- rings ---------- */
static void ring_push(trb_t *ring, int *idx, int *cycle, u64 param, u32 status, u32 control){
    trb_t *t = &ring[*idx];
    t->param = param;
    t->status = status;
    t->control = (control & ~1u) | (u32)*cycle;
    (*idx)++;
    if(*idx == RING_N - 1){
        /* link back to the start and toggle the cycle bit */
        trb_t *l = &ring[RING_N - 1];
        l->param = (u64)(unsigned long)ring;
        l->status = 0;
        l->control = (TRB_LINK << 10) | (1u << 1) | (u32)*cycle;   /* toggle cycle */
        *idx = 0;
        *cycle ^= 1;
    }
}

/* ---------- waiting for events ---------- */
static void handle_event(trb_t *e);

/* Waits for a command completion (want_type EV_CMD_DONE) or for a transfer
   completion on endpoint ep of slot (want_type EV_TRANSFER). */
static int wait_event(int want_type, int slot, int ep, u32 timeout_ms){
    u64 until = timer_ticks() + timeout_ms / 10;
    for(;;){
        if(want_type == EV_CMD_DONE && cmd_done_code != -1) return cmd_done_code;
        if(want_type == EV_TRANSFER && xfer_done_code != -1 && xfer_done_slot == slot && xfer_done_ep == ep)
            return xfer_done_code;
        if(timer_ticks() > until) return -1;
        /* process whatever is ready */
        trb_t *e = &evt_ring[evt_i];
        if((e->control & 1) == (u32)evt_cycle){
            handle_event(e);
            evt_i++;
            if(evt_i == RING_N){ evt_i = 0; evt_cycle ^= 1; }
            /* tell the controller where we are (ERDP, with the event-handler busy bit) */
            wr64(rt, 0x20 + 0x18, (u64)(unsigned long)&evt_ring[evt_i] | (1u << 3));
        } else {
            __asm__ volatile("pause");
        }
    }
}

static void handle_event(trb_t *e){
    u32 type = (e->control >> 10) & 0x3F;
    int code = (int)((e->status >> 24) & 0xFF);
    int slot = (int)((e->control >> 24) & 0xFF);
    int ep = (int)((e->control >> 16) & 0x1F);
    if(type == EV_CMD_DONE){
        cmd_done_code = code;
        cmd_done_slot = slot;
    } else if(type == EV_TRANSFER){
        if(slot >= 1 && slot <= MAX_SLOTS && slots[slot].kbd && ep == slots[slot].kbd_dci){
            /* a report arrived */
            if(code == 1 || code == 13){       /* success / short packet */
                u8 *r = slots[slot].report;
                if(slots[slot].hid_kind == 2){
                    /* boot mouse: buttons, dx, dy (dy is down-positive) */
                    mouse_inject((signed char)r[1], (signed char)r[2], r[0] & 7);
                } else {
                    extern void usb_keyboard_report(const u8 *r, u8 *prev);
                    usb_keyboard_report(r, slots[slot].prev);
                }
            }
            /* re-arm the interrupt endpoint */
            ring_push(slots[slot].kring, &slots[slot].k_i, &slots[slot].k_cycle,
                      (u64)(unsigned long)slots[slot].report, 8,
                      (TRB_NORMAL << 10) | (1u << 5));
            db[slot] = (u32)slots[slot].kbd_dci;
        } else if(slot >= 1 && slot <= MAX_SLOTS && slots[slot].hub && ep == slots[slot].hub_dci){
            /* bit p set = port p changed (ports 1..7 fit in the first byte) */
            if(code == 1 || code == 13) slots[slot].pending = slots[slot].hreport[0];
            ring_push(slots[slot].hring, &slots[slot].h_i, &slots[slot].h_cycle,
                      (u64)(unsigned long)slots[slot].hreport, 1, (TRB_NORMAL << 10) | (1u << 5));
            db[slot] = (u32)slots[slot].hub_dci;
        } else {
            xfer_done_code = code;
            xfer_done_slot = slot;
            xfer_done_ep = ep;
            xfer_done_resid = (int)(e->status & 0xFFFFFF);
        }
    } else if(type == EV_PORT){
        /* port status changes are picked up by the port scan */
    }
}

/* ---------- commands ---------- */
static int command(u64 param, u32 status, u32 control, int *slot_out){
    cmd_done_code = -1;
    cmd_done_slot = -1;
    ring_push(cmd_ring, &cmd_i, &cmd_cycle, param, status, control);
    db[0] = 0;
    int code = wait_event(EV_CMD_DONE, 0, 0, 2000);
    if(slot_out) *slot_out = cmd_done_slot;
    return code;
}

/* ---------- control transfers on EP0 ---------- */
static int control_xfer(int slot, const u8 setup[8], int dir_in, u8 *data, u16 len){
    dev_t_ *d = &slots[slot];
    xfer_done_code = -1;
    xfer_done_slot = -1;
    u64 sp = 0;
    for(int i = 0; i < 8; i++) sp |= (u64)setup[i] << (8 * i);
    u32 trt = len ? (dir_in ? 3u : 2u) : 0u;
    ring_push(d->ep0, &d->ep0_i, &d->ep0_cycle, sp, 8,
              (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if(len){
        ring_push(d->ep0, &d->ep0_i, &d->ep0_cycle, (u64)(unsigned long)data, len,
                  (TRB_DATA << 10) | ((dir_in ? 1u : 0u) << 16));
    }
    /* status stage goes the other way */
    int status_in = len ? !dir_in : 1;
    ring_push(d->ep0, &d->ep0_i, &d->ep0_cycle, 0, 0,
              (TRB_STATUS << 10) | ((u32)status_in << 16) | (1u << 5));
    db[slot] = 1;                       /* DCI 1 = EP0 */
    int code = wait_event(EV_TRANSFER, slot, 1, 2000);
    return code == 1 || code == 13 ? 0 : -1;
}

static int get_descriptor(int slot, u8 type, u8 idx, u8 *buf, u16 len){
    u8 setup[8] = { 0x80, 6, idx, type, 0, 0, (u8)len, (u8)(len >> 8) };
    return control_xfer(slot, setup, 1, buf, len);
}

/* ---------- the port scan and device setup ---------- */
static int speed_to_mps(int speed){
    switch(speed){
        case 2: return 8;        /* low speed */
        case 1: return 64;       /* full speed (8 would also do at first) */
        case 3: return 64;       /* high speed */
        default: return 512;     /* super speed */
    }
}

static int address_device(int slot, int port, int speed, u32 route, int tt_slot, int tt_port){
    memset(in_ctx, 0, 4096);
    u32 *icc = (u32*)in_ctx;
    icc[1] = 0x3;                          /* add slot and EP0 */
    u32 *sc = ctx_at(in_ctx, 1);
    sc[0] = ((u32)speed << 20) | (1u << 27) | (route & 0xFFFFF);   /* speed, route, one entry */
    sc[1] = (u32)(port + 1) << 16;                     /* root hub port (1-based) */
    sc[2] = (u32)(tt_slot & 0xFF) | ((u32)(tt_port & 0xFF) << 8); /* TT for slow devices behind a hub */
    u32 *ep0 = ctx_at(in_ctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | ((u32)speed_to_mps(speed) << 16);   /* control, CErr 3 */
    u64 ring = (u64)(unsigned long)slots[slot].ep0;
    ep0[2] = (u32)ring | 1u;                            /* DCS = 1 */
    ep0[3] = (u32)(ring >> 32);
    ep0[4] = 8;
    dcbaa[slot] = (u64)(unsigned long)slots[slot].dev_ctx;
    return command((u64)(unsigned long)in_ctx, 0, (TRB_ADDRESS_DEVICE << 10) | ((u32)slot << 24), 0);
}

static int configure_hid(int slot, u8 ep_addr, u8 mps, u8 bint){
    dev_t_ *d = &slots[slot];
    int epn = ep_addr & 0xF;
    int dci = epn * 2 + 1;                          /* IN endpoint */
    memset(in_ctx, 0, 4096);
    u32 *icc = (u32*)in_ctx;
    icc[1] = 1u | (1u << dci);                       /* slot + this endpoint */
    u32 *sc = ctx_at(in_ctx, 1);
    sc[0] = ((u32)d->speed << 20) | ((u32)dci << 27) | (d->route & 0xFFFFF);
    sc[1] = (u32)(d->port + 1) << 16;
    if(d->speed != 3 && d->speed != 4 && d->tt_slot)   /* slow device behind a high-speed hub */
        sc[2] = (u32)(d->tt_slot & 0xFF) | ((u32)(d->tt_port & 0xFF) << 8);
    u32 *ec = ctx_at(in_ctx, dci + 1);
    ec[0] = ((u32)(bint ? bint - 1 : 0) << 16);     /* interval (2^n * 125 us) */
    ec[1] = (7u << 3) | (3u << 1) | ((u32)mps << 16);   /* interrupt IN, CErr 3 */
    d->kring = alloc_page_zeroed();
    d->k_i = 0; d->k_cycle = 1;
    u64 ring = (u64)(unsigned long)d->kring;
    ec[2] = (u32)ring | 1u;
    ec[3] = (u32)(ring >> 32);
    ec[4] = 8;
    d->kbd_dci = dci;
    return command((u64)(unsigned long)in_ctx, 0, (TRB_CONFIGURE_EP << 10) | ((u32)slot << 24), 0);
}

static void keyboard_start(int slot, int kind){
    dev_t_ *d = &slots[slot];
    /* boot protocol, so every report has the fixed boot format */
    u8 setup[8] = { 0x21, 0x0B, 0, 0, (u8)d->iface, 0, 0, 0 };
    control_xfer(slot, setup, 0, 0, 0);
    ring_push(d->kring, &d->k_i, &d->k_cycle, (u64)(unsigned long)d->report, 8,
              (TRB_NORMAL << 10) | (1u << 5));
    db[slot] = (u32)d->kbd_dci;
    d->kbd = 1;
    d->hid_kind = kind;
    console_printf("[usb] %s ready (slot %d)\n", kind == 2 ? "mouse" : "keyboard", slot);
}

static void hub_service(int hub_slot, int p);
static int msd_init(int slot);

/* Brings up one device: at a root port (route 0) or behind a hub. */
static void enumerate(int port, u32 route, int speed, int tt_slot, int tt_port, int depth){
    int slot = -1;
    if(command(0, 0, TRB_ENABLE_SLOT << 10, &slot) != 1 || slot < 1 || slot > MAX_SLOTS) return;
    dev_t_ *d = &slots[slot];
    memset(d, 0, sizeof *d);
    d->used = 1; d->port = port; d->speed = speed; d->route = route;
    d->tt_slot = tt_slot; d->tt_port = tt_port;
    d->dev_ctx = alloc_page_zeroed();
    d->ep0 = alloc_page_zeroed();
    d->report = alloc_page_zeroed();
    if(!d->dev_ctx || !d->ep0 || !d->report) return;
    d->ep0_cycle = 1;
    if(address_device(slot, port, speed, route, tt_slot, tt_port) != 1){
        console_printf("[usb] port %d: address failed\n", port + 1); return;
    }

    u8 dd[18];
    if(get_descriptor(slot, 1, 0, dd, 18) != 0){ console_printf("[usb] port %d: no device descriptor\n", port + 1); return; }
    console_printf("[usb] port %d%s: device %04x:%04x class %02x\n", port + 1,
                   depth ? " (behind a hub)" : "", dd[8] | (dd[9] << 8), dd[10] | (dd[11] << 8), dd[4]);
    u8 cfg[256];
    if(get_descriptor(slot, 2, 0, cfg, 9) != 0) return;
    u16 total = (u16)(cfg[2] | (cfg[3] << 8));
    if(total > sizeof cfg) total = sizeof cfg;
    if(get_descriptor(slot, 2, 0, cfg, total) != 0) return;

    /* a hub: its interrupt endpoint reports which downstream ports changed */
    int hub_iface = -1, hub_ep = -1, hub_mps = 8, hub_int = 12;
    int msd_iface = -1, bo_ep = -1, bi_ep = -1, bo_mps = 512, bi_mps = 512;
    int iface = -1, ep_addr = -1, mps = 8, interval = 10, cfg_value = cfg[5], kind = 0;
    for(int i = cfg[0]; i + 1 < total; ){
        int len = cfg[i], type = cfg[i + 1];
        if(len < 2) break;
        if(type == 4){
            if(cfg[i + 5] == 8 && cfg[i + 6] == 6 && cfg[i + 7] == 0x50){ msd_iface = cfg[i + 2]; }
            if(cfg[i + 5] == 9){ hub_iface = cfg[i + 2]; }
            else if(cfg[i + 5] == 3 && cfg[i + 6] == 1 && (cfg[i + 7] == 1 || cfg[i + 7] == 2)){
                iface = cfg[i + 2];
                kind = cfg[i + 7] == 1 ? 1 : 2;
            }
        } else if(type == 5){
            if(msd_iface >= 0 && (cfg[i + 3] & 3) == 2){                 /* bulk */
                if((cfg[i + 2] & 0x80) && bi_ep < 0){ bi_ep = cfg[i + 2]; bi_mps = cfg[i + 4]; }
                if(!(cfg[i + 2] & 0x80) && bo_ep < 0){ bo_ep = cfg[i + 2]; bo_mps = cfg[i + 4]; }
            }
            if(hub_iface >= 0 && hub_ep < 0 && (cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3){
                hub_ep = cfg[i + 2]; hub_mps = cfg[i + 4]; hub_int = cfg[i + 6];
            }
            if(iface >= 0 && ep_addr < 0 && (cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3){
                ep_addr = cfg[i + 2];
                mps = cfg[i + 4];
                interval = cfg[i + 6];
            }
        }
        i += len;
    }
    if(hub_iface >= 0 && hub_ep >= 0 && depth < 2){
        d->hub_dci = (hub_ep & 0xF) * 2 + 1;
        if(configure_hid(slot, (u8)hub_ep, (u8)hub_mps, (u8)hub_int) != 1) return;
        u8 setcfg[8] = { 0x00, 9, (u8)cfg_value, 0, 0, 0, 0, 0 };
        if(control_xfer(slot, setcfg, 0, 0, 0) != 0) return;
        /* hub descriptor: number of ports is byte 2 */
        u8 hd[16];
        u8 hsetup[8] = { 0xA0, 6, 0, 0x29, 0, 0, 16, 0 };
        if(control_xfer(slot, hsetup, 1, hd, 16) != 0) return;
        d->hub = 1;
        d->nports = hd[2];
        d->hring = alloc_page_zeroed();
        d->hreport = alloc_page_zeroed();
        if(!d->hring || !d->hreport) return;
        d->h_i = 0; d->h_cycle = 1;
        u64 ring = (u64)(unsigned long)d->hring;
        /* a second endpoint context for the hub's interrupt IN endpoint */
        memset(in_ctx, 0, 4096);
        u32 *icc = (u32*)in_ctx;
        icc[1] = 1u | (1u << d->hub_dci);
        u32 *sc = ctx_at(in_ctx, 1);
        sc[0] = ((u32)d->speed << 20) | ((u32)d->hub_dci << 27) | (d->route & 0xFFFFF);
        sc[1] = (u32)(d->port + 1) << 16;
        u32 *ec = ctx_at(in_ctx, d->hub_dci + 1);
        ec[0] = (u32)((hub_int ? hub_int - 1 : 0) << 16);
        ec[1] = (7u << 3) | (3u << 1) | ((u32)hub_mps << 16);
        ec[2] = (u32)ring | 1u; ec[3] = (u32)(ring >> 32); ec[4] = 8;
        if(command((u64)(unsigned long)in_ctx, 0, (TRB_CONFIGURE_EP << 10) | ((u32)slot << 24), 0) != 1) return;
        ring_push(d->hring, &d->h_i, &d->h_cycle, (u64)(unsigned long)d->hreport, 1,
                  (TRB_NORMAL << 10) | (1u << 5));
        db[slot] = (u32)d->hub_dci;
        for(int p = 1; p <= d->nports; p++){
            u8 pw[8] = { 0x23, 3, 8, 0, (u8)p, 0, 0, 0 };          /* SET_FEATURE PORT_POWER */
            control_xfer(slot, pw, 0, 0, 0);
        }
        console_printf("[usb] hub with %d ports (slot %d)\n", d->nports, slot);
        /* devices already connected are not always re-announced by the hub, so
           look at every port once the power has come up */
        timer_sleep(20);
        for(int p = 1; p <= d->nports; p++) hub_service(slot, p);
        return;
    }

    if(msd_iface >= 0 && bo_ep >= 0 && bi_ep >= 0){
        /* mass storage: configure both bulk endpoints in one command */
        d->msd = 1;
        d->bo_dci = (bo_ep & 0xF) * 2;
        d->bi_dci = (bi_ep & 0xF) * 2 + 1;
        d->bo_ring = alloc_page_zeroed(); d->bi_ring = alloc_page_zeroed();
        d->cbuf = alloc_page_zeroed(); d->dbuf = alloc_page_zeroed(); d->sbuf = alloc_page_zeroed();
        if(!d->bo_ring || !d->bi_ring || !d->cbuf || !d->dbuf || !d->sbuf) return;
        d->bo_i = d->bi_i = 0; d->bo_c = d->bi_c = 1;
        memset(in_ctx, 0, 4096);
        u32 *icc = (u32*)in_ctx;
        icc[1] = 1u | (1u << d->bo_dci) | (1u << d->bi_dci);
        u32 *sc = ctx_at(in_ctx, 1);
        int top = d->bi_dci > d->bo_dci ? d->bi_dci : d->bo_dci;
        sc[0] = ((u32)d->speed << 20) | ((u32)top << 27) | (d->route & 0xFFFFF);
        sc[1] = (u32)(d->port + 1) << 16;
        if(d->speed != 3 && d->speed != 4 && d->tt_slot)
            sc[2] = (u32)(d->tt_slot & 0xFF) | ((u32)(d->tt_port & 0xFF) << 8);
        u32 *oc = ctx_at(in_ctx, d->bo_dci + 1);
        oc[1] = (2u << 3) | (3u << 1) | ((u32)bo_mps << 16);          /* bulk OUT */
        u64 ro = (u64)(unsigned long)d->bo_ring;
        oc[2] = (u32)ro | 1u; oc[3] = (u32)(ro >> 32); oc[4] = 512;
        u32 *ic = ctx_at(in_ctx, d->bi_dci + 1);
        ic[1] = (6u << 3) | (3u << 1) | ((u32)bi_mps << 16);          /* bulk IN */
        u64 ri = (u64)(unsigned long)d->bi_ring;
        ic[2] = (u32)ri | 1u; ic[3] = (u32)(ri >> 32); ic[4] = 512;
        if(command((u64)(unsigned long)in_ctx, 0, (TRB_CONFIGURE_EP << 10) | ((u32)slot << 24), 0) != 1) return;
        u8 setcfg[8] = { 0x00, 9, (u8)cfg_value, 0, 0, 0, 0, 0 };
        if(control_xfer(slot, setcfg, 0, 0, 0) != 0) return;
        msd_init(slot);
        return;
    }
    if(iface < 0 || ep_addr < 0) return;
    d->iface = iface;
    if(configure_hid(slot, (u8)ep_addr, (u8)mps, (u8)interval) != 1) return;
    u8 setcfg[8] = { 0x00, 9, (u8)cfg_value, 0, 0, 0, 0, 0 };
    if(control_xfer(slot, setcfg, 0, 0, 0) != 0) return;
    keyboard_start(slot, kind);
}

static void attach_device(int port){
    u32 psc = rd32(op, 0x400 + 0x10 * port);
    /* reset the port */
    wr32(op, 0x400 + 0x10 * port, (psc & ~0x00FE0000u) | (1u << 4));
    for(int t = 0; t < 100000 && !(rd32(op, 0x400 + 0x10 * port) & (1u << 21)); t++) ;
    wr32(op, 0x400 + 0x10 * port, rd32(op, 0x400 + 0x10 * port) | (1u << 21));  /* clear PRC */
    psc = rd32(op, 0x400 + 0x10 * port);
    if(!(psc & 1)) return;
    int speed = (int)((psc >> 10) & 0xF);
    enumerate(port, 0, speed, 0, 0, 0);
}

/* A device behind a hub changed: reset and bring up what is connected there. */
static void hub_service(int hub_slot, int p){
    dev_t_ *h = &slots[hub_slot];
    u8 st[4];
    u8 gs[8] = { 0xA3, 0, 0, 0, (u8)p, 0, 4, 0 };          /* GET_STATUS(port) */
    if(control_xfer(hub_slot, gs, 1, st, 4) != 0) return;
    u16 status = (u16)(st[0] | (st[1] << 8)), change = (u16)(st[2] | (st[3] << 8));
    if(change & 1){                                          /* C_PORT_CONNECTION */
        u8 cc[8] = { 0x23, 1, 16, 0, (u8)p, 0, 0, 0 };
        control_xfer(hub_slot, cc, 0, 0, 0);
    }
    if(!(status & 1)) return;                                /* nothing connected */
    u8 pr[8] = { 0x23, 3, 4, 0, (u8)p, 0, 0, 0 };           /* SET_FEATURE PORT_RESET */
    control_xfer(hub_slot, pr, 0, 0, 0);
    for(int t = 0; t < 100; t++){
        timer_sleep(1);
        if(control_xfer(hub_slot, gs, 1, st, 4) == 0 && ((st[2] | (st[3] << 8)) & (1u << 4))) break;
    }
    u8 cr[8] = { 0x23, 1, 20, 0, (u8)p, 0, 0, 0 };          /* C_PORT_RESET */
    control_xfer(hub_slot, cr, 0, 0, 0);
    if(control_xfer(hub_slot, gs, 1, st, 4) != 0) return;
    status = (u16)(st[0] | (st[1] << 8));
    if(!(status & 2)) return;                               /* not enabled */
    int speed = (status & (1u << 9)) ? 2 : (status & (1u << 10)) ? 3 : 1;   /* LS, HS, FS */
    int hub_port_root = h->port;
    /* the route string has one nibble per hub level, the outermost hub first */
    int level = 0;
    for(u32 r = h->route; r; r >>= 4) level++;
    u32 route = h->route | ((u32)(p & 0xF) << (4 * level));
    int tt_slot = (speed != 3 && h->speed == 3) ? hub_slot : 0;
    int tt_port = (speed != 3 && h->speed == 3) ? p : 0;
    enumerate(hub_port_root, route, speed, tt_slot, tt_port, 1);
}

/* ---------- mass storage: bulk-only transport and SCSI ---------- */
static int msd_slot = -1;
static u32 msd_blocks, msd_bsize = 512;
static char msd_vendor[9], msd_product[17];

static void put_be32(u8 *p, u32 v){ p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 get_be32(const u8 *p){ return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* one bulk transfer on ep dci; returns the bytes not transferred, or -1 */
static int bulk(int slot, int dci, trb_t *ring, int *idx, int *cyc, void *buf, u32 len){
    xfer_done_code = -1; xfer_done_slot = -1; xfer_done_ep = -1;
    ring_push(ring, idx, cyc, (u64)(unsigned long)buf, len, (TRB_NORMAL << 10) | (1u << 5));
    db[slot] = (u32)dci;
    int code = wait_event(EV_TRANSFER, slot, dci, 3000);
    if(code != 1 && code != 13) return -1;
    return (int)xfer_done_resid;
}

/* sends a SCSI command block; moves dlen bytes in the given direction */
static int bot(int slot, const u8 *cdb, u8 cdlen, int dir_in, u8 *data, u32 dlen){
    dev_t_ *d = &slots[slot];
    u8 *cbw = d->cbuf;
    memset(cbw, 0, 31);
    put_be32(cbw + 0, 0x55534243u);            /* "USBC" in little-endian byte order below */
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    d->tag++;
    cbw[4] = (u8)d->tag; cbw[5] = (u8)(d->tag >> 8); cbw[6] = (u8)(d->tag >> 16); cbw[7] = (u8)(d->tag >> 24);
    cbw[8] = (u8)dlen; cbw[9] = (u8)(dlen >> 8); cbw[10] = (u8)(dlen >> 16); cbw[11] = (u8)(dlen >> 24);
    cbw[12] = dir_in ? 0x80 : 0;
    cbw[13] = 0;                               /* LUN 0 */
    cbw[14] = cdlen;
    memcpy(cbw + 15, cdb, cdlen);
    if(bulk(slot, d->bo_dci, d->bo_ring, &d->bo_i, &d->bo_c, cbw, 31) < 0) return -1;
    if(dlen){
        if(dir_in){
            if(bulk(slot, d->bi_dci, d->bi_ring, &d->bi_i, &d->bi_c, d->dbuf, dlen) < 0) return -1;
            memcpy(data, d->dbuf, dlen);
        } else {
            memcpy(d->dbuf, data, dlen);
            if(bulk(slot, d->bo_dci, d->bo_ring, &d->bo_i, &d->bo_c, d->dbuf, dlen) < 0) return -1;
        }
    }
    u8 *csw = d->sbuf;
    memset(csw, 0, 13);
    if(bulk(slot, d->bi_dci, d->bi_ring, &d->bi_i, &d->bi_c, csw, 13) < 0) return -1;
    if(csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S') return -1;
    return csw[12] == 0 ? 0 : -1;             /* bCSWStatus: 0 = good */
}

static int msd_init(int slot){
    dev_t_ *d = &slots[slot];
    u8 inq[36];
    u8 cdb_inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if(bot(slot, cdb_inq, 6, 1, inq, 36) != 0) return -1;
    memcpy(msd_vendor, inq + 8, 8); msd_vendor[8] = 0;
    memcpy(msd_product, inq + 16, 16); msd_product[16] = 0;
    u8 cap[8];
    u8 cdb_cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if(bot(slot, cdb_cap, 10, 1, cap, 8) != 0) return -1;
    msd_blocks = get_be32(cap) + 1;
    msd_bsize = get_be32(cap + 4);
    if(!msd_bsize) msd_bsize = 512;
    msd_slot = slot;
    console_printf("[usb] mass storage: %s %s, %u blocks of %u bytes\n", msd_vendor, msd_product,
                   msd_blocks, msd_bsize);
    (void)d;
    return 0;
}

int usb_disk_present(void){ return msd_slot > 0; }

void usb_disk_info(u32 *blocks, u32 *block_size, char vendor[9], char product[17]){
    if(blocks) *blocks = msd_blocks;
    if(block_size) *block_size = msd_bsize;
    if(vendor) memcpy(vendor, msd_vendor, 9);
    if(product) memcpy(product, msd_product, 17);
}

/* reads or writes count blocks at lba, at most 8 per SCSI command */
static int msd_io(u32 lba, u32 count, void *buf, int write){
    if(msd_slot <= 0) return -1;
    u8 *p = buf;
    while(count){
        u32 n = count > 8 ? 8 : count;
        u8 cdb[10] = { (u8)(write ? 0x2A : 0x28), 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        put_be32(cdb + 2, lba);
        cdb[7] = (u8)(n >> 8); cdb[8] = (u8)n;
        if(bot(msd_slot, cdb, 10, !write, p, n * msd_bsize) != 0) return -1;
        p += n * msd_bsize; lba += n; count -= n;
    }
    return 0;
}

int usb_disk_read(u32 lba, u32 count, void *buf){ return msd_io(lba, count, buf, 0); }
int usb_disk_write(u32 lba, u32 count, const void *buf){ return msd_io(lba, count, (void*)buf, 1); }

/* ---------- HID boot report to characters ---------- */
static const char hid_plain[64] = {
    0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l',
    'm','n','o','p','q','r','s','t','u','v','w','x','y','z','1','2',
    '3','4','5','6','7','8','9','0','\n',27,'\b','\t',' ','-','=','[',
    ']','\\',0,';','\'','`',',','.','/',0,0,0,0,0,0,0
};
static const char hid_shift[64] = {
    0,0,0,0,'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','!','@',
    '#','$','%','^','&','*','(',')','\n',27,'\b','\t',' ','_','+','{',
    '}','|',0,':','"','~','<','>','?',0,0,0,0,0,0,0
};

void usb_keyboard_report(const u8 *r, u8 *prev){
    u8 mods = r[0];
    int shift = (mods & 0x22) != 0, ctrl = (mods & 0x11) != 0;
    for(int i = 2; i < 8; i++){
        u8 k = r[i];
        if(!k) continue;
        int was = 0;
        for(int j = 2; j < 8; j++) if(prev[j] == k) was = 1;
        if(was) continue;                       /* held from the last report */
        /* Ctrl+Alt+F1..F9 switch terminals, as on the PS/2 keyboard */
        if(ctrl && (mods & 0x44) && k >= 0x3A && k <= 0x42){
            keyboard_request_vt(k - 0x3A + 1);
            continue;
        }
        int c = 0;
        if(k == 0x4F) c = KEY_RIGHT; else if(k == 0x50) c = KEY_LEFT;
        else if(k == 0x51) c = KEY_DOWN; else if(k == 0x52) c = KEY_UP;
        else if(k == 0x4A) c = KEY_HOME; else if(k == 0x4D) c = KEY_END;
        else if(k == 0x4C) c = KEY_DEL; else if(k == 0x4B) c = KEY_PGUP;
        else if(k == 0x4E) c = KEY_PGDN;
        else if(k < 64){
            c = shift ? hid_shift[k] : hid_plain[k];
            if(ctrl && c >= 'a' && c <= 'z') c = c - 'a' + 1;
        }
        if(c) keyboard_inject(c);
    }
    for(int i = 0; i < 8; i++) prev[i] = r[i];
}

/* called from the keyboard path while waiting for keys */
static void usb_poll(void){
    if(!op) return;
    trb_t *e = &evt_ring[evt_i];
    if((e->control & 1) == (u32)evt_cycle){
        handle_event(e);
        evt_i++;
        if(evt_i == RING_N){ evt_i = 0; evt_cycle ^= 1; }
        wr64(rt, 0x20 + 0x18, (u64)(unsigned long)&evt_ring[evt_i] | (1u << 3));
    }
    /* hub changes are serviced here, not in the event handler: service makes
       control transfers, which need the event loop */
    for(int h = 1; h <= MAX_SLOTS; h++){
        if(!slots[h].hub || !slots[h].pending) continue;
        int p = slots[h].pending;
        slots[h].pending = 0;
        for(int k = 1; k <= slots[h].nports; k++){
            if(!(p & (1 << k))) continue;
            hub_service(h, k);
        }
    }
}

/* ---------- controller setup ---------- */
int xhci_init(void){
    pci_dev_t *d = pci_find_class(0x0C, 0x03);
    if(!d || d->prog_if != 0x30) return -1;
    pci_enable(d);
    u64 base = pci_bar_base(d, 0);
    if(!base) return -1;
    vmm_map_region(base, base, 0x20000);
    cap = (volatile u8*)(unsigned long)base;
    u8 caplen = cap[0];
    op = cap + caplen;
    u32 hcs1 = rd32(cap, 0x04);
    u32 hccs1 = rd32(cap, 0x10);
    u32 dboff = rd32(cap, 0x14) & ~3u;
    u32 rtsoff = rd32(cap, 0x18) & ~0x1Fu;
    rt = cap + rtsoff;
    db = (volatile u32*)(cap + dboff);
    ctx64 = (hccs1 >> 2) & 1;
    max_ports = (int)(hcs1 >> 24);
    int max_slots = (int)(hcs1 & 0xFF);

    /* reset the controller */
    wr32(op, 0x00, rd32(op, 0x00) & ~1u);                 /* stop */
    for(int t = 0; t < 100000 && !(rd32(op, 0x04) & 1); t++) ;
    wr32(op, 0x00, rd32(op, 0x00) | 2u);                  /* HCRST */
    for(int t = 0; t < 1000000 && (rd32(op, 0x00) & 2); t++) ;
    for(int t = 0; t < 1000000 && (rd32(op, 0x04) & (1u << 11)); t++) ;

    dcbaa = alloc_page_zeroed();
    cmd_ring = alloc_page_zeroed();
    evt_ring = alloc_page_zeroed();
    erst = alloc_page_zeroed();
    in_ctx = alloc_page_zeroed();
    if(!dcbaa || !cmd_ring || !evt_ring || !erst || !in_ctx) return -1;
    if(max_slots > MAX_SLOTS) max_slots = MAX_SLOTS;
    wr32(op, 0x38, (u32)max_slots);                       /* CONFIG: slots enabled */
    wr64(op, 0x30, (u64)(unsigned long)dcbaa);            /* DCBAAP */
    wr64(op, 0x18, (u64)(unsigned long)cmd_ring | 1u);    /* CRCR with cycle 1 */

    /* event ring: one segment */
    erst[0] = (u64)(unsigned long)evt_ring;
    erst[1] = RING_N;
    wr32(rt, 0x20 + 0x08, 1);                              /* ERSTSZ */
    wr64(rt, 0x20 + 0x18, (u64)(unsigned long)evt_ring);   /* ERDP */
    wr64(rt, 0x20 + 0x10, (u64)(unsigned long)erst);       /* ERSTBA */

    cmd_i = 0; cmd_cycle = 1; evt_i = 0; evt_cycle = 1;
    wr32(op, 0x00, rd32(op, 0x00) | 1u);                  /* run */
    for(int t = 0; t < 100000 && (rd32(op, 0x04) & 1); t++) ;
    cmd_done_code = -1;

    keyboard_poll_hook = usb_poll;
    console_printf("[xhci] %d ports, %d slots\n", max_ports, max_slots);

    /* attach whatever is plugged in */
    for(int p = 0; p < max_ports; p++){
        u32 psc = rd32(op, 0x400 + 0x10 * p);
        if(psc & 1) attach_device(p);
    }
    return 0;
}
