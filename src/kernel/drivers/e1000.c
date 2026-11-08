/* e1000.c — Intel 82540EM-family gigabit NIC (QEMU's "e1000"). Polled, no IRQs. */
#include "amdroid/nic.h"
#include "amdroid/pci.h"
#include "amdroid/vmm.h"
#include "amdroid/pmm.h"
#include "amdroid/console.h"
#include "amdroid/timer.h"
#include "amdroid/types.h"
#include <string.h>

#define CTRL   0x0000
#define STATUS 0x0008
#define RCTL   0x0100
#define TCTL   0x0400
#define RDBAL  0x2800
#define RDBAH  0x2804
#define RDLEN  0x2808
#define RDH    0x2810
#define RDT    0x2818
#define TDBAL  0x3800
#define TDBAH  0x3804
#define TDLEN  0x3808
#define TDH    0x3810
#define TDT    0x3818
#define RAL0   0x5400
#define RAH0   0x5404

#define N 16
#define BUFSZ 2048

typedef struct { u64 addr; u16 len; u16 csum; u8 status; u8 errors; u16 special; } __attribute__((packed)) rxdesc_t;
typedef struct { u64 addr; u16 len; u8 cso; u8 cmd; u8 status; u8 css; u16 special; } __attribute__((packed)) txdesc_t;

static volatile u8 *mmio;
static rxdesc_t *rx;
static txdesc_t *tx;
static u8 *rxbuf[N], *txbuf[N];
static int rx_next, tx_next;
static nic_t nic_e1000;

static inline void wr(u32 off, u32 v){ *(volatile u32*)(mmio + off) = v; }
static inline u32 rd(u32 off){ return *(volatile u32*)(mmio + off); }

static int e1000_send(const u8 *frame, u32 len){
    if(len > BUFSZ) return -1;
    int i = tx_next;
    /* wait until this slot is free (the device sets DD when it is done with it) */
    for(int t = 0; t < 1000000 && !(tx[i].status & 1) && tx[i].cmd; t++) ;
    memcpy(txbuf[i], frame, len);
    tx[i].addr = (u64)(unsigned long)txbuf[i];
    tx[i].len = (u16)len;
    tx[i].cmd = 0x0B;               /* EOP | IFCS | RS */
    tx[i].status = 0;
    tx_next = (i + 1) % N;
    wr(TDT, (u32)tx_next);
    return 0;
}

static int e1000_recv(u8 *frame, u32 max, u32 *len){
    rxdesc_t *d = &rx[rx_next];
    if(!(d->status & 1)) return 0;
    u32 n = d->len;
    if(n > max) n = max;
    memcpy(frame, rxbuf[rx_next], n);
    *len = n;
    d->status = 0;
    wr(RDT, (u32)rx_next);          /* give the slot back to the card */
    rx_next = (rx_next + 1) % N;
    return 1;
}

int e1000_init(void){
    pci_dev_t *d = pci_find(0x8086, 0x100E);
    if(!d) return -1;
    pci_enable(d);
    u64 base = pci_bar_base(d, 0);
    if(!base) return -1;
    vmm_map_region(base, base, 0x20000);
    mmio = (volatile u8*)(unsigned long)base;

    wr(CTRL, rd(CTRL) | (1u << 26));            /* reset */
    for(int t = 0; t < 1000000 && (rd(CTRL) & (1u << 26)); t++) ;
    wr(CTRL, rd(CTRL) | (1u << 6) | (1u << 5)); /* link up, auto speed */
    timer_sleep(2);

    u32 ral = rd(RAL0), rah = rd(RAH0);
    nic_e1000.mac[0] = (u8)ral; nic_e1000.mac[1] = (u8)(ral >> 8);
    nic_e1000.mac[2] = (u8)(ral >> 16); nic_e1000.mac[3] = (u8)(ral >> 24);
    nic_e1000.mac[4] = (u8)rah; nic_e1000.mac[5] = (u8)(rah >> 8);

    rx = pmm_alloc_page();
    tx = pmm_alloc_page();
    if(!rx || !tx) return -1;
    memset(rx, 0, 4096); memset(tx, 0, 4096);
    for(int i = 0; i < N; i++){
        rxbuf[i] = pmm_alloc_page();
        txbuf[i] = pmm_alloc_page();
        if(!rxbuf[i] || !txbuf[i]) return -1;
        rx[i].addr = (u64)(unsigned long)rxbuf[i];
        tx[i].status = 1;           /* free */
    }
    wr(RDBAL, (u32)(unsigned long)rx); wr(RDBAH, 0);
    wr(RDLEN, N * sizeof(rxdesc_t));
    wr(RDH, 0); wr(RDT, N - 1);
    rx_next = 0;
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));   /* enable, accept broadcast, strip CRC */

    wr(TDBAL, (u32)(unsigned long)tx); wr(TDBAH, 0);
    wr(TDLEN, N * sizeof(txdesc_t));
    wr(TDH, 0); wr(TDT, 0);
    tx_next = 0;
    wr(TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));

    nic_e1000.name = "e1000";
    nic_e1000.send = e1000_send;
    nic_e1000.recv = e1000_recv;
    console_printf("[e1000] %02x:%02x:%02x:%02x:%02x:%02x\n", nic_e1000.mac[0], nic_e1000.mac[1],
                   nic_e1000.mac[2], nic_e1000.mac[3], nic_e1000.mac[4], nic_e1000.mac[5]);
    return 0;
}

nic_t *e1000_nic(void){ return &nic_e1000; }
