/* rtl8169.c — Realtek gigabit / fast Ethernet with descriptor rings: RTL8169, and the
 * PCIe RTL8111/8168 (by far the most common onboard NIC in desktop PCs) and RTL8101/8102/
 * 8103/8105/8106 (RTL810x fast Ethernet). Driven through the I/O-port BAR, polled.
 * The PHY is left to auto-negotiate as the firmware configured it. */
#include "amdroid/nic.h"
#include "amdroid/pci.h"
#include "amdroid/pmm.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

#define IDR0      0x00
#define MAR0      0x08
#define TNPDS     0x20          /* transmit descriptor ring, 64-bit */
#define CHIPCMD   0x37
#define TXPOLL    0x38
#define INTRMASK  0x3C
#define INTRSTAT  0x3E
#define TXCONFIG  0x40
#define RXCONFIG  0x44
#define CFG9346   0x50
#define PHYSTATUS 0x6C
#define RXMAXSIZE 0xDA
#define CPLUSCMD  0xE0
#define RDSAR     0xE4          /* receive descriptor ring, 64-bit */
#define MAXTXSIZE 0xEC

#define CMD_RST 0x10
#define CMD_RE  0x08
#define CMD_TE  0x04

#define OWN (1u << 31)
#define EOR (1u << 30)
#define FS  (1u << 29)
#define LS  (1u << 28)
#define RES (1u << 21)          /* receive error summary */

#define NRX 256                 /* 16-byte descriptors: one page per ring */
#define NTX 64
#define BUFSZ 2048

typedef struct { u32 opts1, opts2; u64 addr; } __attribute__((packed)) desc_t;

static u16 io;
static desc_t *rx, *tx;
static u8 *rxbuf[NRX], *txbuf[NTX];
static int rx_next, tx_next;
static nic_t nic_rtl;
static char nic_name[24];

static inline void ob(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void ow(u16 p, u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void ol(u16 p, u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u8  ib(u16 p){ u8 v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u16 iw(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u32 il(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static int rtl_send(const u8 *frame, u32 len){
    if(len > BUFSZ) return -1;
    int i = tx_next;
    volatile desc_t *d = &tx[i];
    for(int t = 0; t < 1000000 && (d->opts1 & OWN); t++) ;
    if(d->opts1 & OWN) return -1;
    memcpy(txbuf[i], frame, len);
    if(len < 60){ memset(txbuf[i] + len, 0, 60 - len); len = 60; }
    d->addr = (u64)(unsigned long)txbuf[i];
    d->opts2 = 0;
    __asm__ volatile("sfence" ::: "memory");
    d->opts1 = OWN | FS | LS | (i == NTX - 1 ? EOR : 0) | len;
    tx_next = (i + 1) % NTX;
    __asm__ volatile("sfence" ::: "memory");
    ob(io + TXPOLL, 0x40);                      /* normal-priority queue: go */
    return 0;
}

static void rx_give(int i){
    rx[i].opts2 = 0;
    rx[i].addr = (u64)(unsigned long)rxbuf[i];
    __asm__ volatile("sfence" ::: "memory");
    rx[i].opts1 = OWN | (i == NRX - 1 ? EOR : 0) | BUFSZ;
}

static int rtl_recv(u8 *frame, u32 max, u32 *len){
    u16 st = iw(io + INTRSTAT);
    if(st) ow(io + INTRSTAT, st);               /* we poll: just acknowledge */
    for(;;){
        volatile desc_t *d = &rx[rx_next];
        u32 o = d->opts1;
        if(o & OWN) return 0;
        __asm__ volatile("lfence" ::: "memory");
        int good = (o & FS) && (o & LS) && !(o & RES);
        u32 n = o & 0x3FFF;
        if(good && n >= 4){
            n -= 4;                             /* the card leaves the CRC on */
            if(n > max) n = max;
            memcpy(frame, rxbuf[rx_next], n);
            *len = n;
        } else good = 0;
        rx_give(rx_next);
        rx_next = (rx_next + 1) % NRX;
        if(good) return 1;
    }
}

static int rtl_link(void){ return (ib(io + PHYSTATUS) & 0x02) ? 1 : 0; }

static pci_dev_t *find(void){
    static const u16 ids[][2] = {
        {0x10EC,0x8161}, {0x10EC,0x8167}, {0x10EC,0x8168}, {0x10EC,0x8169}, {0x10EC,0x8136},
        {0x10EC,0x2502},                   /* Killer E2500 (8168 inside) */
        {0x1186,0x4300}, {0x1186,0x4302}, /* D-Link DGE-528T */
        {0x1259,0xC107}, /* Corega */
        {0x16EC,0x0116}, /* US Robotics */
        {0x1737,0x1032}, /* Linksys */
    };
    for(u32 k = 0; k < sizeof ids / sizeof ids[0]; k++){
        pci_dev_t *d = pci_find(ids[k][0], ids[k][1]);
        if(d) return d;
    }
    return 0;
}

int rtl8169_init(void){
    pci_dev_t *d = find();
    if(!d) return -1;
    pci_enable(d);
    int b;
    for(b = 0; b < 6; b++) if(d->bar[b] & 1) break;   /* first I/O BAR */
    if(b == 6) return -1;
    io = (u16)(d->bar[b] & ~3u);

    ow(io + INTRMASK, 0);
    ob(io + CHIPCMD, CMD_RST);
    for(int t = 0; t < 1000 && (ib(io + CHIPCMD) & CMD_RST); t++) timer_sleep(1);
    if(ib(io + CHIPCMD) & CMD_RST) return -1;

    u8 *m = nic_rtl.mac;
    for(int i = 0; i < 6; i++) m[i] = ib(io + IDR0 + i);

    rx = pmm_alloc_page();
    tx = pmm_alloc_page();
    if(!rx || !tx) return -1;
    memset(rx, 0, 4096); memset(tx, 0, 4096);
    for(int i = 0; i < NRX; i++){
        if(!rxbuf[i]) rxbuf[i] = pmm_alloc_page();
        if(!rxbuf[i]) return -1;
        rx_give(i);
    }
    for(int i = 0; i < NTX; i++){
        if(!txbuf[i]) txbuf[i] = pmm_alloc_page();
        if(!txbuf[i]) return -1;
    }
    tx[NTX - 1].opts1 = EOR;
    rx_next = tx_next = 0;

    ob(io + CFG9346, 0xC0);                     /* unlock config registers */
    /* C+ mode: PCI multiple read/write on; receive checksum and VLAN untagging off */
    ow(io + CPLUSCMD, (u16)((iw(io + CPLUSCMD) & ~((1u << 5) | (1u << 6))) | (1u << 3)));
    ow(io + RXMAXSIZE, BUFSZ - 1);
    ob(io + MAXTXSIZE, 0x3B);                   /* 0x3B * 128 bytes: anything up to jumbo-ish */
    u64 ta = (u64)(unsigned long)tx, ra = (u64)(unsigned long)rx;
    ol(io + TNPDS + 4, (u32)(ta >> 32)); ol(io + TNPDS, (u32)ta);
    ol(io + RDSAR + 4, (u32)(ra >> 32)); ol(io + RDSAR, (u32)ra);
    ob(io + CHIPCMD, CMD_RE | CMD_TE);
    /* standard inter-frame gap, unlimited DMA burst */
    ol(io + TXCONFIG, (3u << 24) | (7u << 8));
    /* no FIFO threshold, unlimited DMA burst, accept broadcast + multicast + our address */
    ol(io + RXCONFIG, (7u << 13) | (7u << 8) | 0x0E);
    ol(io + MAR0, 0xFFFFFFFF); ol(io + MAR0 + 4, 0xFFFFFFFF);
    ob(io + CFG9346, 0x00);
    ow(io + INTRSTAT, 0xFFFF);

    u32 ver = il(io + TXCONFIG) & 0x7CF00000u;   /* hardware version bits, for the log */
    strcpy(nic_name, d->device == 0x8136 ? "rtl810x" : d->device == 0x8169 ? "rtl8169" : "rtl8168");
    nic_rtl.name = nic_name;
    nic_rtl.send = rtl_send;
    nic_rtl.recv = rtl_recv;
    nic_rtl.link = rtl_link;
    console_printf("[%s] %04x rev %08x  %02x:%02x:%02x:%02x:%02x:%02x\n", nic_name, d->device, ver,
                   m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

nic_t *rtl8169_nic(void){ return &nic_rtl; }
