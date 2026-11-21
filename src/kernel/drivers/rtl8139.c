/* rtl8139.c — Realtek RTL8139 fast Ethernet (QEMU "-device rtl8139", older PCs and cheap
 * PCI cards). Receive is one 8 KB ring the card writes into; transmit is four buffers used in
 * turn. I/O-port BAR, polled. */
#include "amdroid/nic.h"
#include "amdroid/pci.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

#define IDR0    0x00
#define MAR0    0x08
#define TSD0    0x10
#define TSAD0   0x20
#define RBSTART 0x30
#define CR      0x37
#define CAPR    0x38
#define IMR     0x3C
#define ISR     0x3E
#define TCR     0x40
#define RCR     0x44
#define CONFIG1 0x52
#define BMSR    0x64

#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04
#define CR_BUFE 0x01

#define TSD_OWN (1u << 13)      /* set by the card when the buffer has been copied out */

#define RXLEN 8192
#define TXSZ  1792

/* physically contiguous (the kernel image is identity mapped); 16 bytes of slack plus a
 * full frame after the ring because WRAP lets the card run past the end */
static u8 rxring[RXLEN + 16 + 1536] __attribute__((aligned(16)));
static u8 txb[4][TXSZ] __attribute__((aligned(16)));
static int txused[4];
static u16 io;
static u32 rxoff;
static int tx_next;
static nic_t nic_8139;

static inline void ob(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void ow(u16 p, u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void ol(u16 p, u32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline u8  ib(u16 p){ u8 v;  __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u16 iw(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline u32 il(u16 p){ u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static int r39_send(const u8 *frame, u32 len){
    if(len > TXSZ) return -1;
    int i = tx_next;
    u16 tsd = (u16)(io + TSD0 + 4 * i);
    if(txused[i]) for(int t = 0; t < 1000000 && !(il(tsd) & TSD_OWN); t++) ;
    memcpy(txb[i], frame, len);
    if(len < 60){ memset(txb[i] + len, 0, 60 - len); len = 60; }
    ol((u16)(io + TSAD0 + 4 * i), (u32)(unsigned long)txb[i]);
    ol(tsd, len);                               /* OWN clear + size: send */
    txused[i] = 1;
    tx_next = (i + 1) & 3;
    return 0;
}

static int r39_recv(u8 *frame, u32 max, u32 *len){
    u16 st = iw(io + ISR);
    if(st) ow(io + ISR, st);
    while(!(ib(io + CR) & CR_BUFE)){
        u8 *p = rxring + rxoff;
        u16 status = (u16)(p[0] | (p[1] << 8));
        u16 n = (u16)(p[2] | (p[3] << 8));      /* includes the CRC */
        if(n == 0xFFF0) return 0;               /* the card is still writing this one */
        int good = (status & 1) && n >= 64 && n <= 1522;
        if(!good && (n < 8 || n > 1522 + 4)){
            /* ring out of step: restart the receiver */
            ob(io + CR, CR_TE);
            rxoff = 0;
            ol(io + RBSTART, (u32)(unsigned long)rxring);
            ob(io + CR, CR_RE | CR_TE);
            return 0;
        }
        u32 m = n - 4;
        if(good){
            if(m > max) m = max;
            memcpy(frame, p + 4, m);
            *len = m;
        }
        rxoff = (rxoff + n + 4 + 3) & ~3u;
        rxoff %= RXLEN;
        ow(io + CAPR, (u16)(rxoff - 16));
        if(good) return 1;
    }
    return 0;
}

static int r39_link(void){ return (iw(io + BMSR) & 0x04) ? 1 : 0; }

int rtl8139_init(void){
    pci_dev_t *d = pci_find(0x10EC, 0x8139);
    if(!d) d = pci_find(0x10EC, 0x8138);
    if(!d) d = pci_find(0x1113, 0x1211);        /* Accton / SMC clones */
    if(!d) d = pci_find(0x1186, 0x1300);        /* D-Link DFE-530TX+ */
    if(!d) return -1;
    pci_enable(d);
    if(!(d->bar[0] & 1)) return -1;
    io = (u16)(d->bar[0] & ~3u);

    ob(io + CONFIG1, 0);                        /* wake up */
    ob(io + CR, CR_RST);
    for(int t = 0; t < 1000 && (ib(io + CR) & CR_RST); t++) timer_sleep(1);
    if(ib(io + CR) & CR_RST) return -1;

    u8 *m = nic_8139.mac;
    for(int i = 0; i < 6; i++) m[i] = ib(io + IDR0 + i);

    memset(rxring, 0, sizeof rxring);
    rxoff = 0; tx_next = 0;
    memset(txused, 0, sizeof txused);
    ol(io + RBSTART, (u32)(unsigned long)rxring);
    ow(io + IMR, 0);
    ow(io + ISR, 0xFFFF);
    ob(io + CR, CR_RE | CR_TE);
    /* 8 KB ring, WRAP, unlimited DMA, no threshold; broadcast + multicast + our address */
    ol(io + RCR, (7u << 13) | (7u << 8) | (1u << 7) | 0x0E);
    ol(io + TCR, (3u << 24) | (6u << 8));       /* standard IFG, 1 KB DMA burst */
    ol(io + MAR0, 0xFFFFFFFF); ol(io + MAR0 + 4, 0xFFFFFFFF);

    nic_8139.name = "rtl8139";
    nic_8139.send = r39_send;
    nic_8139.recv = r39_recv;
    nic_8139.link = r39_link;
    console_printf("[rtl8139] %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

nic_t *rtl8139_nic(void){ return &nic_8139; }
